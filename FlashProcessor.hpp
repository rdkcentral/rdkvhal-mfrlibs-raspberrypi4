/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2026 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * @author Arun Madhavan
 */

#pragma once
#include "FlashConfig.hpp"
#include "WicStreamFlasher.hpp"
#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <memory>
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <set>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <system_error>
#include <sstream>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sched.h>
#include <sys/syscall.h>
#include <linux/loop.h>
#include <linux/magic.h>
#include <archive.h>
#include <archive_entry.h>

#include "mfrTypes.h"

namespace fs = std::filesystem;

/**
 * @brief Reports mfrWriteImage() style upgrade status through mfrUpgradeStatusNotify_t.
 * Phase changes are reported immediately, with the current phase and percentage repeated
 * every notify.interval seconds. An interval of 0 reports only the final result.
 * finish() always delivers exactly one final
 * mfrUPGRADE_PROGRESS_COMPLETED or mfrUPGRADE_PROGRESS_ABORTED callback.
 */
class UpgradeStatusReporter {
private:
    const mfrUpgradeStatusNotify_t m_notify;
    std::mutex m_mutex;
    std::mutex m_callback_mutex;
    std::condition_variable m_stop_condition;
    std::thread m_periodic_thread;
    bool m_stop_requested = false;
    bool m_is_finished = false;
    mfrUpgradeProgress_t m_progress = mfrUPGRADE_PROGRESS_NOT_STARTED;
    int m_percentage = 0;

    void deliver(mfrUpgradeProgress_t progress, mfrError_t error, const std::string& error_string, int percentage)
    {
        mfrUpgradeStatus_t status {};
        status.progress = progress;
        status.error = error;
        status.percentage = percentage;
        std::snprintf(status.error_string, sizeof(status.error_string), "%s", error_string.c_str());
        m_notify.cb(&status);
    }

    void report_current_status()
    {
        std::lock_guard<std::mutex> callback_lock(m_callback_mutex);
        mfrUpgradeProgress_t progress;
        int percentage;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stop_requested) {
                return;
            }
            progress = m_progress;
            percentage = m_percentage;
        }
        deliver(progress, mfrERR_NONE, "", percentage);
    }

    void report_periodically()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (!m_stop_condition.wait_for(lock, std::chrono::seconds(m_notify.interval), [this] { return m_stop_requested; })) {
            lock.unlock();
            report_current_status();
            lock.lock();
        }
    }

    void stop_periodic_reports()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop_requested = true;
        }
        m_stop_condition.notify_all();
        if (m_periodic_thread.joinable()) {
            m_periodic_thread.join();
        }
    }

public:
    explicit UpgradeStatusReporter(const mfrUpgradeStatusNotify_t& notify) : m_notify(notify)
    {
        if (m_notify.cb && m_notify.interval > 0) {
            m_periodic_thread = std::thread(&UpgradeStatusReporter::report_periodically, this);
        }
    }

    ~UpgradeStatusReporter() { stop_periodic_reports(); }

    UpgradeStatusReporter(const UpgradeStatusReporter&) = delete;
    UpgradeStatusReporter& operator=(const UpgradeStatusReporter&) = delete;

    void set_phase(mfrUpgradeProgress_t progress, int percentage)
    {
        // Serialize phase changes with periodic callbacks so an old phase cannot arrive later.
        std::lock_guard<std::mutex> callback_lock(m_callback_mutex);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stop_requested) {
                return;
            }
            m_progress = progress;
            m_percentage = std::max(m_percentage, std::clamp(percentage, 0, 99));
            percentage = m_percentage;
        }
        if (m_notify.cb && m_notify.interval > 0) {
            deliver(progress, mfrERR_NONE, "", percentage);
        }
    }

    void set_percentage(int percentage)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_stop_requested) {
            m_percentage = std::max(m_percentage, std::clamp(percentage, 0, 99));
        }
    }

    void set_byte_progress(std::uintmax_t completed_bytes, std::uintmax_t total_bytes,
                           int start_percentage, int end_percentage)
    {
        if (total_bytes == 0) {
            return;
        }
        const long double fraction = static_cast<long double>(std::min(completed_bytes, total_bytes)) /
                                     static_cast<long double>(total_bytes);
        const int percentage = start_percentage + static_cast<int>(fraction * (end_percentage - start_percentage));
        // Reserve the endpoint until the operation has also finalized successfully.
        set_percentage(std::min(percentage, end_percentage - 1));
    }

    void finish(mfrError_t error, const std::string& error_string)
    {
        stop_periodic_reports();
        std::lock_guard<std::mutex> callback_lock(m_callback_mutex);
        int percentage;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_is_finished) {
                return;
            }
            m_is_finished = true;
            percentage = m_percentage;
        }
        if (!m_notify.cb) {
            return;
        }
        if (error == mfrERR_NONE) {
            deliver(mfrUPGRADE_PROGRESS_COMPLETED, mfrERR_NONE, "", 100);
        } else {
            deliver(mfrUPGRADE_PROGRESS_ABORTED, error, error_string, percentage);
        }
    }
};

class NativeFlashProcessor {
private:
    std::string m_image_path;
    std::string m_loop_device_path;
    int m_loop_control_file_descriptor = FlashConfig::INVALID_FILE_DESCRIPTOR;
    int m_loop_device_file_descriptor = FlashConfig::INVALID_FILE_DESCRIPTOR;
    int m_lock_file_descriptor = FlashConfig::INVALID_FILE_DESCRIPTOR;
    std::string m_boot_image_staging_path;

    std::atomic<bool> m_is_critical_update_phase{false};
    std::atomic<bool> m_has_deferred_signal{false};
    std::atomic<int> m_deferred_signal_number{0};

    /**
     * @brief Apply reduced CPU and I/O priority, and pin the process to specific CPU cores.
     * 1. Reduce CPU priority using setpriority (equivalent to nice -n 10).
     * 2. Reduce I/O priority to Idle using ioprio_set (equivalent to ionice -c 3).
     * 3. Pin the process to CPU cores 2 and 3 using sched_setaffinity (equivalent to taskset -c 2,3).
     */
    void apply_reduced_priority()
    {
        std::cout << FlashConfig::LOG_TAG << "Applying reduced CPU and I/O priority, and pinning to cores 2 and 3." << std::endl;
        // 1. Reduce CPU priority natively (Equivalent to nice -n 10)
        ::setpriority(PRIO_PROCESS, 0, FlashConfig::CPU_NICE_LOW_PRIORITY);

        // 2. Reduce I/O priority to Idle (Equivalent to ionice -c 3)
        int io_priority_value = (FlashConfig::IO_PRIORITY_CLASS_IDLE << FlashConfig::IO_PRIORITY_INFO_SHIFT);
        ::syscall(SYS_ioprio_set, FlashConfig::IO_PRIORITY_PROCESS, 0, io_priority_value);

        // 3. Pin task processing threads strictly to cores 2 and 3 (Equivalent to taskset -c 2,3)
        cpu_set_t cpu_affinity_set;
        CPU_ZERO(&cpu_affinity_set);
        CPU_SET(FlashConfig::CPU_AFFINITY_CORE_2, &cpu_affinity_set);
        CPU_SET(FlashConfig::CPU_AFFINITY_CORE_3, &cpu_affinity_set);
        ::sched_setaffinity(0, sizeof(cpu_set_t), &cpu_affinity_set);
    }

    /**
     * @brief Lock the instance of the application to prevent concurrent execution.
     * If the lock cannot be acquired, it indicates that another instance is already running.
     * @return true if the lock was successfully acquired, false otherwise.
     */
    bool lock_instance()
    {
        m_lock_file_descriptor = ::open(FlashConfig::INSTANCE_LOCK_FILE, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
        if (m_lock_file_descriptor < 0) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to open lock file: " << FlashConfig::INSTANCE_LOCK_FILE << std::endl;
            return false;
        }
        if (::flock(m_lock_file_descriptor, LOCK_EX | LOCK_NB) < 0) {
            ::close(m_lock_file_descriptor);
            m_lock_file_descriptor = FlashConfig::INVALID_FILE_DESCRIPTOR;
            std::cerr << FlashConfig::LOG_TAG << "Another instance of the application is already running. Exiting." << std::endl;
            return false;
        }
        return true;
    }

    /**
     * @brief Check if the provided image file is a GZIP archive by reading its header.
     * @param image_path The path to the image file.
     * @return true if the file is a GZIP archive, false otherwise.
     */
    bool is_gzip_archive(const std::string& image_path)
    {
        std::ifstream image_stream(image_path, std::ios::binary);
        if (!image_stream) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to open image file: " << image_path << std::endl;
            return false;
        }
        unsigned char gzip_header[FlashConfig::GZIP_HEADER_SIZE] = {0};
        image_stream.read(reinterpret_cast<char*>(gzip_header), FlashConfig::GZIP_HEADER_SIZE);
        return (image_stream.gcount() == FlashConfig::GZIP_HEADER_SIZE &&
                gzip_header[0] == FlashConfig::GZIP_HEADER_BYTE_1 &&
                gzip_header[1] == FlashConfig::GZIP_HEADER_BYTE_2);
    }

    /**
     * @brief Set up a loop device for the provided image file.
     * @param image_path The path to the image file.
     * @return true if the loop device was successfully set up, false otherwise.
     */
    bool setup_loop_device(const std::string& image_path)
    {
        m_loop_control_file_descriptor = ::open(FlashConfig::LOOP_CONTROL_DEVICE, O_RDWR);
        if (m_loop_control_file_descriptor < 0) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to open loop control device: " << FlashConfig::LOOP_CONTROL_DEVICE << std::endl;
            return false;
        }
        int loop_device_number = ::ioctl(m_loop_control_file_descriptor, LOOP_CTL_GET_FREE);
        if (loop_device_number < 0) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to get free loop device number." << std::endl;
            return false;
        }
        m_loop_device_path = std::string(FlashConfig::LOOP_DEVICE_PREFIX) + std::to_string(loop_device_number);
        m_loop_device_file_descriptor = ::open(m_loop_device_path.c_str(), O_RDWR);
        if (m_loop_device_file_descriptor < 0) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to open loop device: " << m_loop_device_path << std::endl;
            return false;
        }
        int image_file_descriptor = ::open(image_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (image_file_descriptor < 0) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to open image file: " << image_path << std::endl;
            return false;
        }
        if (::ioctl(m_loop_device_file_descriptor, LOOP_SET_FD, image_file_descriptor) < 0) {
            ::close(image_file_descriptor);
            std::cerr << FlashConfig::LOG_TAG << "Failed to associate image file with loop device: " << m_loop_device_path << std::endl;
            return false;
        }
        ::close(image_file_descriptor);
        return true;
    }

    /**
     * @brief Determine the passive root filesystem device based on the current active device.
     * This function reads the command line from /proc/cmdline to identify the active root filesystem device.
     * @return The path to the passive root filesystem device.
     */
    bool get_rootfs_devices(std::string& active_rootfs_device, std::string& passive_rootfs_device)
    {
        std::ifstream proc_command_line(FlashConfig::PROC_COMMAND_LINE_PATH);
        std::string command_line;
        if (!proc_command_line || !std::getline(proc_command_line, command_line)) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to read active root device from "
                      << FlashConfig::PROC_COMMAND_LINE_PATH << std::endl;
            return false;
        }

        std::istringstream command_line_stream(command_line);
        std::string argument;
        std::string root_argument;
        while (command_line_stream >> argument) {
            if (argument.rfind("root=", 0) == 0) {
                if (!root_argument.empty()) {
                    std::cerr << FlashConfig::LOG_TAG << "Multiple root= arguments found in kernel command line." << std::endl;
                    return false;
                }
                root_argument = argument.substr(5);
            }
        }
        if (root_argument == FlashConfig::ROOTFS_BANK_A_DEVICE) {
            active_rootfs_device = FlashConfig::ROOTFS_BANK_A_DEVICE;
            passive_rootfs_device = FlashConfig::ROOTFS_BANK_B_DEVICE;
            return true;
        }
        if (root_argument == FlashConfig::ROOTFS_BANK_B_DEVICE) {
            active_rootfs_device = FlashConfig::ROOTFS_BANK_B_DEVICE;
            passive_rootfs_device = FlashConfig::ROOTFS_BANK_A_DEVICE;
            return true;
        }
        std::cerr << FlashConfig::LOG_TAG << "Unsupported or missing root= device in kernel command line: "
                  << (root_argument.empty() ? "<missing>" : root_argument) << std::endl;
        return false;
    }

    std::uintmax_t get_tree_size(const fs::path& source_path)
    {
        std::set<std::pair<dev_t, ino_t>> counted_files;
        const auto calculate_size = [&counted_files](const auto& self, const fs::path& path) -> std::uintmax_t {
            struct stat file_status {};
            if (::lstat(path.c_str(), &file_status) != 0) {
                throw fs::filesystem_error("Failed to inspect copy source", path,
                                           std::error_code(errno, std::generic_category()));
            }

            if (S_ISDIR(file_status.st_mode)) {
                std::uintmax_t total_bytes = 0;
                for (const auto& directory_entry : fs::directory_iterator(path)) {
                    total_bytes += self(self, directory_entry.path());
                }
                return total_bytes;
            }

            if (S_ISREG(file_status.st_mode) && counted_files.emplace(file_status.st_dev, file_status.st_ino).second) {
                return static_cast<std::uintmax_t>(file_status.st_size);
            }
            return 0;
        };
        return calculate_size(calculate_size, source_path);
    }

    /**
     * @brief Copy a filesystem tree while preserving its on-disk metadata.
     * @param source_path The path to the source directory or file.
     * @param destination_path The path to the destination directory.
     * @param report_copied_bytes Optional callback receiving logical bytes copied.
     */
    void copy_tree(const fs::path& source_path, const fs::path& destination_path,
                   const std::function<void(std::uintmax_t)>& report_copied_bytes = {})
    {
        const fs::path source_root = fs::absolute(source_path).lexically_normal();
        const fs::path destination_root = fs::absolute(destination_path).lexically_normal();
        if (source_root == destination_root) {
            throw fs::filesystem_error("Copy source and destination are the same path", source_root, destination_root,
                                       std::make_error_code(std::errc::file_exists));
        }

        fs::create_directories(destination_root.parent_path());
        const fs::path destination_filesystem_path = fs::exists(destination_root)
            ? destination_root : destination_root.parent_path();
        struct statfs destination_filesystem {};
        if (::statfs(destination_filesystem_path.c_str(), &destination_filesystem) != 0) {
            throw fs::filesystem_error("Failed to inspect copy destination filesystem", destination_filesystem_path,
                                       std::error_code(errno, std::generic_category()));
        }

        std::unique_ptr<struct archive, decltype(&archive_read_free)> archive_reader(archive_read_disk_new(), archive_read_free);
        std::unique_ptr<struct archive, decltype(&archive_write_free)> archive_writer(archive_write_disk_new(), archive_write_free);
        std::unique_ptr<struct archive_entry, decltype(&archive_entry_free)> archive_entry(archive_entry_new(), archive_entry_free);
        if (!archive_reader || !archive_writer || !archive_entry) {
            throw std::runtime_error("Failed to allocate filesystem copy resources");
        }

        const auto archive_error = [](struct archive* archive_handle, const char* operation) {
            std::string message(operation);
            const char* error_message = archive_error_string(archive_handle);
            if (error_message) {
                message += ": ";
                message += error_message;
            }
            return std::runtime_error(message);
        };
        const auto map_source_path = [&source_root, &destination_root](const fs::path& path) {
            const fs::path normalized_path = path.lexically_normal();
            const fs::path relative_path = normalized_path.lexically_relative(source_root);
            if (relative_path.empty() && normalized_path != source_root) {
                throw std::runtime_error("Copy source entry escaped the source tree");
            }
            if (relative_path.is_absolute() || (!relative_path.empty() && *relative_path.begin() == "..")) {
                throw std::runtime_error("Copy source entry escaped the source tree");
            }
            return (destination_root / relative_path).lexically_normal();
        };

        int archive_status = archive_read_disk_set_symlink_physical(archive_reader.get());
        if (archive_status != ARCHIVE_OK) {
            throw archive_error(archive_reader.get(), "Failed to configure filesystem traversal");
        }

        int extract_options = ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_ACL |
            ARCHIVE_EXTRACT_XATTR | ARCHIVE_EXTRACT_OWNER | ARCHIVE_EXTRACT_FFLAGS |
            ARCHIVE_EXTRACT_UNLINK | ARCHIVE_EXTRACT_SECURE_SYMLINKS;
        if (destination_filesystem.f_type == MSDOS_SUPER_MAGIC) {
            extract_options &= ~(ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_ACL | ARCHIVE_EXTRACT_XATTR |
                                 ARCHIVE_EXTRACT_OWNER | ARCHIVE_EXTRACT_FFLAGS);
        }
        archive_status = archive_write_disk_set_options(archive_writer.get(), extract_options);
        if (archive_status != ARCHIVE_OK) {
            throw archive_error(archive_writer.get(), "Failed to configure filesystem copy");
        }
        archive_status = archive_read_disk_open(archive_reader.get(), source_root.c_str());
        if (archive_status != ARCHIVE_OK) {
            throw archive_error(archive_reader.get(), "Failed to open copy source");
        }

        while (true) {
            archive_status = archive_read_next_header2(archive_reader.get(), archive_entry.get());
            if (archive_status == ARCHIVE_EOF) {
                break;
            }
            if (archive_status != ARCHIVE_OK) {
                throw archive_error(archive_reader.get(), "Failed to read copy source entry");
            }

            const fs::path source_entry_path = archive_entry_pathname(archive_entry.get());
            if (const char* hardlink_path = archive_entry_hardlink(archive_entry.get())) {
                fs::path source_hardlink_path = hardlink_path;
                if (!source_hardlink_path.is_absolute()) {
                    source_hardlink_path = source_root / source_hardlink_path;
                }
                const std::string destination_hardlink_path = map_source_path(source_hardlink_path).string();
                archive_entry_set_hardlink(archive_entry.get(), destination_hardlink_path.c_str());
            }
            const fs::path destination_entry_path = map_source_path(source_entry_path);
            const std::string destination_entry_name = destination_entry_path.string();
            archive_entry_set_pathname(archive_entry.get(), destination_entry_name.c_str());

            archive_status = archive_write_header(archive_writer.get(), archive_entry.get());
            if (archive_status != ARCHIVE_OK) {
                throw archive_error(archive_writer.get(), "Failed to create copy destination entry");
            }

            const bool report_entry_progress = report_copied_bytes &&
                archive_entry_filetype(archive_entry.get()) == AE_IFREG &&
                archive_entry_hardlink(archive_entry.get()) == nullptr;
            std::uintmax_t copied_entry_bytes = 0;
            while (true) {
                const void* data_block = nullptr;
                size_t data_block_size = 0;
                la_int64_t data_block_offset = 0;
                archive_status = archive_read_data_block(archive_reader.get(), &data_block, &data_block_size, &data_block_offset);
                if (archive_status == ARCHIVE_EOF) {
                    break;
                }
                if (archive_status != ARCHIVE_OK) {
                    throw archive_error(archive_reader.get(), "Failed to read copy source data");
                }
                archive_status = archive_write_data_block(archive_writer.get(), data_block, data_block_size, data_block_offset);
                if (archive_status != ARCHIVE_OK) {
                    throw archive_error(archive_writer.get(), "Failed to write copy destination data");
                }
                if (report_entry_progress && data_block_offset >= 0) {
                    const auto block_end = static_cast<std::uintmax_t>(data_block_offset) + data_block_size;
                    if (block_end > copied_entry_bytes) {
                        report_copied_bytes(block_end - copied_entry_bytes);
                        copied_entry_bytes = block_end;
                    }
                }
            }

            archive_status = archive_write_finish_entry(archive_writer.get());
            if (archive_status != ARCHIVE_OK) {
                throw archive_error(archive_writer.get(), "Failed to finish copy destination entry");
            }
            if (report_entry_progress && archive_entry_size(archive_entry.get()) > 0) {
                const auto entry_size = static_cast<std::uintmax_t>(archive_entry_size(archive_entry.get()));
                if (entry_size > copied_entry_bytes) {
                    report_copied_bytes(entry_size - copied_entry_bytes);
                }
            }
            if (archive_read_disk_can_descend(archive_reader.get()) > 0) {
                archive_status = archive_read_disk_descend(archive_reader.get());
                if (archive_status != ARCHIVE_OK) {
                    throw archive_error(archive_reader.get(), "Failed to descend into copy source directory");
                }
            }
        }

        archive_status = archive_read_close(archive_reader.get());
        if (archive_status != ARCHIVE_OK) {
            throw archive_error(archive_reader.get(), "Failed to close copy source");
        }
        archive_status = archive_write_close(archive_writer.get());
        if (archive_status != ARCHIVE_OK) {
            throw archive_error(archive_writer.get(), "Failed to finalize copy destination metadata");
        }
    }

    void copy_tree_with_progress(const fs::path& source_path, const fs::path& destination_path,
                                 UpgradeStatusReporter& status_reporter, int start_percentage, int end_percentage)
    {
        const std::uintmax_t total_bytes = get_tree_size(source_path);
        std::uintmax_t copied_bytes = 0;
        copy_tree(source_path, destination_path, [&](std::uintmax_t bytes) {
            copied_bytes += bytes;
            status_reporter.set_byte_progress(copied_bytes, total_bytes, start_percentage, end_percentage);
        });
        status_reporter.set_percentage(end_percentage);
    }

    /**
     * @brief Flush all pending writes on the boot filesystem to the card.
     * @return 0 on success, otherwise the errno value of the failed operation.
     */
    static int sync_boot_filesystem()
    {
        const int boot_directory_descriptor = ::open(
            FlashConfig::BOOT_DIRECTORY, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (boot_directory_descriptor < 0) {
            return errno;
        }
        const int sync_error = ::syncfs(boot_directory_descriptor) == 0 ? 0 : errno;
        ::close(boot_directory_descriptor);
        return sync_error;
    }

    /**
     * @brief Replace the contents of the boot directory with the provided source directory.
     * @param source_directory The directory whose contents will be copied into the boot directory.
     * @param status_reporter Optional progress reporter; omitted when restoring the backup.
     */
    void replace_boot_contents(const fs::path& source_directory, UpgradeStatusReporter* status_reporter = nullptr)
    {
        for (auto& directory_entry : fs::directory_iterator(FlashConfig::BOOT_DIRECTORY)) {
            fs::remove_all(directory_entry.path());
        }
        if (status_reporter) {
            copy_tree_with_progress(source_directory, FlashConfig::BOOT_DIRECTORY, *status_reporter, 80, 90);
        } else {
            copy_tree(source_directory, FlashConfig::BOOT_DIRECTORY);
        }
        const int sync_error = sync_boot_filesystem();
        if (sync_error != 0) {
            throw fs::filesystem_error("Failed to synchronize boot filesystem",
                                       FlashConfig::BOOT_DIRECTORY,
                                       std::error_code(sync_error, std::generic_category()));
        }
    }

    /**
     * @brief Restore the boot directory from the backup taken before flashing.
     * @return true if the backup was restored, false otherwise.
     */
    bool restore_boot_backup()
    {
        try {
            replace_boot_contents(FlashConfig::BOOT_BACKUP_DIRECTORY);
            std::cout << FlashConfig::LOG_TAG << "Restored " << FlashConfig::BOOT_DIRECTORY << " from backup." << std::endl;
            return true;
        } catch (const std::exception& exception) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to restore " << FlashConfig::BOOT_DIRECTORY
                      << " from backup; manual recovery required: " << exception.what() << std::endl;
            // Persist whatever was restored so a power loss does not leave /boot in an even older state.
            const int sync_error = sync_boot_filesystem();
            if (sync_error != 0) {
                std::cerr << FlashConfig::LOG_TAG << "Failed to synchronize partially restored "
                          << FlashConfig::BOOT_DIRECTORY << ": " << std::strerror(sync_error) << std::endl;
            }
            return false;
        }
    }

    /**
     * @brief Point the boot command line at the newly flashed root filesystem device.
     * @param active_rootfs_device The currently running root filesystem device.
     * @param passive_rootfs_device The newly flashed root filesystem device.
     * @return true if the boot command line refers to the passive device, false otherwise.
     */
    bool update_boot_command_line(const std::string& active_rootfs_device, const std::string& passive_rootfs_device)
    {
        std::ifstream command_line_input(FlashConfig::BOOT_COMMAND_LINE_PATH);
        if (!command_line_input) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to read boot command line: " << FlashConfig::BOOT_COMMAND_LINE_PATH << std::endl;
            return false;
        }
        std::string command_line_contents((std::istreambuf_iterator<char>(command_line_input)),
                                          std::istreambuf_iterator<char>());
        command_line_input.close();

        const auto is_space = [](char character) {
            return character == ' ' || character == '\t' || character == '\r' || character == '\n';
        };
        std::size_t root_argument_count = 0;
        std::size_t active_argument_count = 0;
        std::size_t passive_argument_count = 0;
        for (std::size_t position = 0; position < command_line_contents.size();) {
            while (position < command_line_contents.size() && is_space(command_line_contents[position])) {
                ++position;
            }
            const std::size_t token_start = position;
            while (position < command_line_contents.size() && !is_space(command_line_contents[position])) {
                ++position;
            }
            if (command_line_contents.compare(token_start, 5, "root=") != 0) {
                continue;
            }
            ++root_argument_count;
            const std::size_t value_start = token_start + 5;
            const std::size_t value_size = position - value_start;
            const std::string root_device = command_line_contents.substr(value_start, value_size);
            if (root_device == active_rootfs_device) {
                command_line_contents.replace(value_start, value_size, passive_rootfs_device);
                ++active_argument_count;
            } else if (root_device == passive_rootfs_device) {
                ++passive_argument_count;
            } else {
                std::cerr << FlashConfig::LOG_TAG << "Boot command line contains an unsupported root= device: "
                          << root_device << std::endl;
                return false;
            }
        }
        if (root_argument_count != 1 || (active_argument_count + passive_argument_count) != 1) {
            std::cerr << FlashConfig::LOG_TAG << "Boot command line must contain exactly one root= entry for an A/B bank."
                      << std::endl;
            return false;
        }
        if (active_argument_count == 0) {
            std::cout << FlashConfig::LOG_TAG << "Boot command line already uses "
                      << passive_rootfs_device << "." << std::endl;
            return true;
        }

        std::string temporary_path = std::string(FlashConfig::BOOT_COMMAND_LINE_PATH) + ".tmp.XXXXXX";
        std::vector<char> temporary_path_buffer(temporary_path.begin(), temporary_path.end());
        temporary_path_buffer.push_back('\0');
        const int temporary_file_descriptor = ::mkstemp(temporary_path_buffer.data());
        if (temporary_file_descriptor < 0) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to create a temporary boot command line: "
                      << std::strerror(errno) << std::endl;
            return false;
        }
        const std::string actual_temporary_path(temporary_path_buffer.data());
        bool write_succeeded = true;
        std::size_t bytes_written = 0;
        while (bytes_written < command_line_contents.size()) {
            const ssize_t write_result = ::write(temporary_file_descriptor,
                                                 command_line_contents.data() + bytes_written,
                                                 command_line_contents.size() - bytes_written);
            if (write_result < 0 && errno == EINTR) {
                continue;
            }
            if (write_result <= 0) {
                write_succeeded = false;
                break;
            }
            bytes_written += static_cast<std::size_t>(write_result);
        }
        if (write_succeeded && ::fsync(temporary_file_descriptor) != 0) {
            write_succeeded = false;
        }
        int write_error = write_succeeded ? 0 : errno;
        if (::close(temporary_file_descriptor) != 0 && write_succeeded) {
            write_succeeded = false;
            write_error = errno;
        }
        if (!write_succeeded || ::rename(actual_temporary_path.c_str(), FlashConfig::BOOT_COMMAND_LINE_PATH) != 0) {
            const int error_number = write_succeeded ? errno : write_error;
            ::unlink(actual_temporary_path.c_str());
            std::cerr << FlashConfig::LOG_TAG << "Failed to atomically update boot command line: "
                      << std::strerror(error_number) << std::endl;
            return false;
        }
        const int sync_error = sync_boot_filesystem();
        if (sync_error != 0) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to synchronize boot command line update: "
                      << std::strerror(sync_error) << std::endl;
            return false;
        }
        std::cout << FlashConfig::LOG_TAG << "Boot command line updated to use " << passive_rootfs_device << "." << std::endl;
        return true;
    }

    /**
     * @brief Run the flashing steps after the instance lock and image existence checks.
     * @param status_reporter Receives phase changes and measured work progress.
     * @param failure_reason Set to a short description when a step fails.
     * @return mfrERR_NONE on success, otherwise the error describing the failed step.
     */
    mfrError_t perform_flash(UpgradeStatusReporter& status_reporter, std::string& failure_reason)
    {
        const auto fail = [&failure_reason](mfrError_t error, const std::string& reason) {
            failure_reason = reason;
            return error;
        };

        apply_reduced_priority();
        status_reporter.set_phase(mfrUPGRADE_PROGRESS_VERIFYING, 0);

        std::string active_rootfs_device;
        std::string passive_rootfs_device;
        if (!get_rootfs_devices(active_rootfs_device, passive_rootfs_device)) {
            return fail(mfrERR_WRITE_FLASH_FAILED, "Unable to identify active and passive rootfs banks");
        }
        std::cout << FlashConfig::LOG_TAG << "Active root filesystem: " << active_rootfs_device
                  << "; passive root filesystem: " << passive_rootfs_device << std::endl;

        // The passive bank (e.g. /root2) is mounted read-write at boot; raw writes require it unmounted.
        std::string unmount_error;
        int unmount_error_code = 0;
        if (!unmount_rootfs_bank(passive_rootfs_device, unmount_error, unmount_error_code)) {
            std::cerr << FlashConfig::LOG_TAG << unmount_error << std::endl;
            return fail(mfrERR_WRITE_FLASH_FAILED, "Passive root filesystem could not be unmounted");
        }

        // Prepare the boot directory for flashing by creating a backup and staging the new contents.
        fs::create_directories(FlashConfig::BOOT_MOUNT_DIRECTORY);
        fs::remove_all(FlashConfig::BOOT_BACKUP_DIRECTORY);
        fs::create_directories(FlashConfig::BOOT_BACKUP_DIRECTORY);
        copy_tree_with_progress(FlashConfig::BOOT_DIRECTORY, FlashConfig::BOOT_BACKUP_DIRECTORY,
                                status_reporter, 0, 40);
        status_reporter.set_percentage(40);

        // Stream the WIC image to the passive rootfs partition.
        m_is_critical_update_phase.store(true, std::memory_order_release);
        status_reporter.set_phase(mfrUPGRADE_PROGRESS_FLASHING, 45);
        const auto stream_start = std::chrono::steady_clock::now();
        const WicStreamResult stream_result = stream_wic_image(
            m_image_path, passive_rootfs_device,
            [&status_reporter](std::uint64_t written, std::uint64_t total) {
                status_reporter.set_byte_progress(written, total, 40, 80);
            });
        const auto stream_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - stream_start).count();
        if (!stream_result.success) {
            std::cerr << FlashConfig::LOG_TAG << "Raw WIC streaming failed: " << stream_result.message
                      << " (error " << stream_result.error_code << ")." << std::endl;
            const mfrError_t error = stream_result.error_code == EINVAL
                ? mfrERR_BAD_IMAGE_HEADER : mfrERR_WRITE_FLASH_FAILED;
            return fail(error, stream_result.message);
        }
        m_boot_image_staging_path = stream_result.boot_image_path;
        std::cout << FlashConfig::LOG_TAG << "Streamed " << stream_result.bytes_written
                  << " partition bytes in " << stream_duration << " ms." << std::endl;
        status_reporter.set_percentage(80);

        if (!setup_loop_device(m_boot_image_staging_path)) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to set up loop device for staged boot image: "
                      << std::strerror(errno) << std::endl;
            return fail(mfrERR_SRC_FILE_ERROR, "Boot image loop setup failed");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(FlashConfig::LOOP_DEVICE_STABILIZATION_DELAY_MS));

        // Mount the staged boot partition to update the boot files.
        if (::mount(m_loop_device_path.c_str(),
                    FlashConfig::BOOT_MOUNT_DIRECTORY,
                    FlashConfig::VFAT_FILESYSTEM_TYPE, MS_RDONLY, nullptr) != 0) {
            const int error_number = errno;
            std::cerr << FlashConfig::LOG_TAG << "Failed to mount staged boot partition: "
                      << m_loop_device_path << ": " << std::strerror(error_number) << std::endl;
            return fail(mfrERR_BAD_IMAGE_HEADER, "Staged boot partition mount failed");
        }
        try {
            replace_boot_contents(FlashConfig::BOOT_MOUNT_DIRECTORY, &status_reporter);
        } catch (const std::exception& exception) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to update " << FlashConfig::BOOT_DIRECTORY
                      << ": " << exception.what() << std::endl;
            ::umount(FlashConfig::BOOT_MOUNT_DIRECTORY);
            restore_boot_backup();
            return fail(mfrERR_WRITE_FLASH_FAILED, "Boot partition update failed");
        }
        if (::umount(FlashConfig::BOOT_MOUNT_DIRECTORY) != 0) {
            const int error_number = errno;
            std::cerr << FlashConfig::LOG_TAG << "Failed to unmount staged boot partition: "
                      << std::strerror(error_number) << std::endl;
            restore_boot_backup();
            return fail(mfrERR_WRITE_FLASH_FAILED, "Staged boot partition unmount failed");
        }

        status_reporter.set_percentage(90);
        if (!update_boot_command_line(active_rootfs_device, passive_rootfs_device)) {
            restore_boot_backup();
            return fail(mfrERR_UPDATE_BOOT_PARAMS_FAILED, "Boot cmdline update failed");
        }
        status_reporter.set_percentage(99);

        // Clean up the OTA archive if it was a GZIP file and the flashing process completed successfully.
        if (is_gzip_archive(m_image_path)) {
            std::error_code remove_error;
            fs::remove(m_image_path, remove_error);
            if (remove_error) {
                std::cerr << FlashConfig::LOG_TAG << "Flashing succeeded, but failed to remove OTA archive "
                          << m_image_path << ": " << remove_error.message() << std::endl;
            }
        }
        return mfrERR_NONE;
    }

public:
    explicit NativeFlashProcessor(const std::string& image_path) : m_image_path(image_path) {}
    ~NativeFlashProcessor() noexcept { cleanup(); }

    /**
     * @brief Handle termination signals (SIGINT, SIGTERM, SIGHUP).
     * If a critical update phase is in progress, the signal is deferred until the update completes.
     * Otherwise, the application performs cleanup and exits immediately.
     */
    void handle_signal(int signal_number)
    {
        if (m_is_critical_update_phase.load(std::memory_order_acquire)) {
            std::cout << FlashConfig::LOG_TAG << "Flash in progress. Deferring termination signal (" << signal_number << ")." << std::endl;
            m_has_deferred_signal.store(true, std::memory_order_release);
            m_deferred_signal_number.store(signal_number, std::memory_order_release);
        } else {
            std::cout << FlashConfig::LOG_TAG << "Termination signal (" << signal_number << ") received. Cleaning up and exiting." << std::endl;
            cleanup(); std::exit(signal_number);
        }
    }

    /**
     * @brief Stream the passive rootfs partition, update boot files, then switch root banks.
     * Errors detected before flashing starts are returned without invoking notify.cb.
     * With a positive interval, notify.cb reports NOT_STARTED, STARTED, VERIFYING, and FLASHING,
     * followed by COMPLETED or ABORTED. Periodic callbacks report the current phase and measured
     * percentage. An interval of 0 reports only the final result. The call blocks until flashing finishes.
     * @param notify The upgrade status callback configuration.
     * @return mfrERR_NONE if the flashing process was successful, otherwise the mfrError_t describing the failure.
     */
    mfrError_t flash(const mfrUpgradeStatusNotify_t& notify)
    {
        if (notify.interval < 0) {
            std::cerr << FlashConfig::LOG_TAG << "Notification interval cannot be negative." << std::endl;
            return mfrERR_INVALID_PARAM;
        }
        if (!lock_instance()) {
            return mfrERR_OPERATION_NOT_SUPPORTED;
        }

        std::error_code exists_error;
        const bool image_exists = fs::exists(m_image_path, exists_error);
        if (exists_error || !image_exists) {
            std::cerr << FlashConfig::LOG_TAG << "Cannot access image file " << m_image_path;
            if (exists_error) {
                std::cerr << ": " << exists_error.message();
            } else {
                std::cerr << ": file does not exist";
            }
            std::cerr << std::endl;
            return mfrERR_IMAGE_FILE_OPEN_FAILED;
        }
        std::error_code type_error;
        if (!fs::is_regular_file(m_image_path, type_error) || type_error) {
            std::cerr << FlashConfig::LOG_TAG << "Image path is not a regular file: " << m_image_path << std::endl;
            return mfrERR_IMAGE_FILE_OPEN_FAILED;
        }

        UpgradeStatusReporter status_reporter(notify);
        status_reporter.set_phase(mfrUPGRADE_PROGRESS_NOT_STARTED, 0);
        status_reporter.set_phase(mfrUPGRADE_PROGRESS_STARTED, 0);
        std::string failure_reason;
        mfrError_t flash_result = mfrERR_NONE;
        try {
            flash_result = perform_flash(status_reporter, failure_reason);
        } catch (const std::exception& exception) {
            std::cerr << FlashConfig::LOG_TAG << "Flashing failed with exception: " << exception.what() << std::endl;
            failure_reason = "Flash I/O error";
            flash_result = mfrERR_WRITE_FLASH_FAILED;
        }
        m_is_critical_update_phase.store(false, std::memory_order_release);

        if (flash_result == mfrERR_NONE) {
            std::cout << FlashConfig::LOG_TAG << "Flashing process completed successfully." << std::endl;
        } else {
            std::cerr << FlashConfig::LOG_TAG << "Flashing process failed: " << failure_reason
                      << " (error " << flash_result << ")." << std::endl;
            cleanup();
        }
        status_reporter.finish(flash_result, failure_reason);

        if (m_has_deferred_signal.load(std::memory_order_acquire)) {
            std::cout << FlashConfig::LOG_TAG << "Processing deferred signal (" << m_deferred_signal_number.load() << ")." << std::endl;
            cleanup();
            std::exit(m_deferred_signal_number.load());
        }
        return flash_result;
    }

    /**
     * @brief Perform cleanup operations, including unmounting directories, releasing loop devices,
     * closing file descriptors, and removing temporary files and directories.
     */
    void cleanup() noexcept
    {
        const int unmount_result = ::umount(FlashConfig::BOOT_MOUNT_DIRECTORY);
        const int unmount_error = unmount_result == 0 ? 0 : errno;
        const bool temporary_mount_root_can_be_removed =
            unmount_result == 0 || unmount_error == EINVAL || unmount_error == ENOENT;
        if (!temporary_mount_root_can_be_removed) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to unmount staged boot partition during cleanup: "
                      << std::strerror(unmount_error) << std::endl;
        }
        bool staged_image_can_be_removed = true;
        if (m_loop_device_file_descriptor >= 0) {
            if (::ioctl(m_loop_device_file_descriptor, LOOP_CLR_FD, 0) < 0 && errno != ENXIO) {
                std::cerr << FlashConfig::LOG_TAG << "Failed to detach loop device "
                          << m_loop_device_path << ": " << std::strerror(errno) << std::endl;
                staged_image_can_be_removed = false;
            }
            ::close(m_loop_device_file_descriptor);
            m_loop_device_file_descriptor = FlashConfig::INVALID_FILE_DESCRIPTOR;
        }
        if (m_loop_control_file_descriptor >= 0) {
            ::close(m_loop_control_file_descriptor);
            m_loop_control_file_descriptor = FlashConfig::INVALID_FILE_DESCRIPTOR;
        }
        if (m_lock_file_descriptor >= 0) {
            ::flock(m_lock_file_descriptor, LOCK_UN);
            ::close(m_lock_file_descriptor);
            m_lock_file_descriptor = FlashConfig::INVALID_FILE_DESCRIPTOR;
        }
        if (staged_image_can_be_removed && !m_boot_image_staging_path.empty()) {
            if (::unlink(m_boot_image_staging_path.c_str()) != 0 && errno != ENOENT) {
                std::cerr << FlashConfig::LOG_TAG << "Failed to remove staged boot image "
                          << m_boot_image_staging_path << ": " << std::strerror(errno) << std::endl;
            } else {
                m_boot_image_staging_path.clear();
            }
        }
        if (temporary_mount_root_can_be_removed) {
            std::error_code remove_error;
            fs::remove_all(FlashConfig::TEMPORARY_MOUNT_ROOT, remove_error);
            if (remove_error) {
                std::cerr << FlashConfig::LOG_TAG << "Failed to remove temporary mount directory: "
                          << remove_error.message() << std::endl;
            }
        }
    }
};
