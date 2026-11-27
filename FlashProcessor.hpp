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
#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <memory>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <system_error>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sched.h>
#include <sys/syscall.h>
#include <linux/loop.h>
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
     * @brief Check if the provided image file has a valid MBR signature.
     * @param image_path The path to the image file.
     * @return true if the file has a valid MBR signature, false otherwise.
     */
    bool has_mbr_signature(const std::string& image_path)
    {
        bool has_signature = false;
        std::ifstream image_stream(image_path, std::ios::binary);
        if (!image_stream) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to open image file: " << image_path << std::endl;
            return false;
        }
        image_stream.seekg(FlashConfig::MBR_SIGNATURE_OFFSET, std::ios::beg);
        unsigned char mbr_signature[FlashConfig::MBR_SIGNATURE_SIZE] = {0};
        image_stream.read(reinterpret_cast<char*>(mbr_signature), FlashConfig::MBR_SIGNATURE_SIZE);
        has_signature = (image_stream.gcount() == FlashConfig::MBR_SIGNATURE_SIZE &&
                mbr_signature[0] == FlashConfig::MBR_SIGNATURE_BYTE_1 &&
                mbr_signature[1] == FlashConfig::MBR_SIGNATURE_BYTE_2);
        std::cout << FlashConfig::LOG_TAG << "Image file " << image_path << (has_signature ? " has a valid MBR signature." : " does not have a valid MBR signature.") << std::endl;
        return has_signature;
    }

    /**
     * @brief Unpack a GZIP archive to the specified destination directory using libarchive.
     * @param archive_path The path to the GZIP archive.
     * @param destination_directory The directory where the archive contents will be extracted.
     * @param status_reporter Receives progress based on compressed bytes consumed.
     * @return true if the extraction was successful, false otherwise.
     */
    bool unpack_archive(const std::string& archive_path, const std::string& destination_directory,
                        UpgradeStatusReporter& status_reporter)
    {
        const std::uintmax_t archive_size = fs::file_size(archive_path);
        std::unique_ptr<struct archive, decltype(&archive_read_free)> archive_reader(archive_read_new(), archive_read_free);
        std::unique_ptr<struct archive, decltype(&archive_write_free)> archive_writer(archive_write_disk_new(), archive_write_free);
        if (!archive_reader || !archive_writer) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to allocate archive reader or writer: " << archive_path << std::endl;
            return false;
        }

        const auto log_archive_error = [&archive_path](struct archive* archive_handle, const char* operation, auto archive_status) {
            std::cerr << FlashConfig::LOG_TAG << operation << ": " << archive_path << " (status " << archive_status;
            const char* error_message = archive_error_string(archive_handle);
            if (error_message) {
                std::cerr << ", " << error_message;
            }
            std::cerr << ")" << std::endl;
        };

        int archive_status = archive_write_disk_set_options(archive_writer.get(), ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM);
        if (archive_status != ARCHIVE_OK) {
            log_archive_error(archive_writer.get(), "Failed to configure archive extraction", archive_status);
            return false;
        }
        archive_status = archive_read_support_filter_gzip(archive_reader.get());
        if (archive_status != ARCHIVE_OK) {
            log_archive_error(archive_reader.get(), "Failed to enable GZIP support", archive_status);
            return false;
        }
        archive_status = archive_read_support_format_tar(archive_reader.get());
        if (archive_status != ARCHIVE_OK) {
            log_archive_error(archive_reader.get(), "Failed to enable TAR support", archive_status);
            return false;
        }
        archive_status = archive_read_open_filename(archive_reader.get(), archive_path.c_str(), FlashConfig::ARCHIVE_READ_BLOCK_SIZE);
        if (archive_status != ARCHIVE_OK) {
            log_archive_error(archive_reader.get(), "Failed to open archive", archive_status);
            return false;
        }

        struct archive_entry* archive_entry;
        while (true) {
            archive_status = archive_read_next_header(archive_reader.get(), &archive_entry);
            if (archive_status == ARCHIVE_EOF) {
                break;
            }
            if (archive_status != ARCHIVE_OK) {
                log_archive_error(archive_reader.get(), "Failed to read archive entry header", archive_status);
                return false;
            }
            fs::path extracted_entry_path = fs::path(destination_directory) / archive_entry_pathname(archive_entry);
            archive_entry_set_pathname(archive_entry, extracted_entry_path.c_str());
            archive_status = archive_write_header(archive_writer.get(), archive_entry);
            if (archive_status != ARCHIVE_OK) {
                log_archive_error(archive_writer.get(), "Failed to write archive entry header", archive_status);
                return false;
            }
            if (archive_entry_size(archive_entry) > 0) {
                const void* data_block;
                size_t data_block_size;
                la_int64_t data_block_offset;
                while (true) {
                    archive_status = archive_read_data_block(archive_reader.get(), &data_block, &data_block_size, &data_block_offset);
                    if (archive_status == ARCHIVE_EOF) {
                        break;
                    }
                    if (archive_status != ARCHIVE_OK) {
                        log_archive_error(archive_reader.get(), "Failed to read archive entry data", archive_status);
                        return false;
                    }
                    const auto write_status = archive_write_data_block(archive_writer.get(), data_block, data_block_size, data_block_offset);
                    if (write_status != ARCHIVE_OK) {
                        log_archive_error(archive_writer.get(), "Failed to write archive entry data", write_status);
                        return false;
                    }
                    const auto compressed_bytes_read = archive_filter_bytes(archive_reader.get(), -1);
                    if (compressed_bytes_read < 0) {
                        std::cerr << FlashConfig::LOG_TAG << "Failed to read archive byte count: " << archive_path << std::endl;
                        return false;
                    }
                    status_reporter.set_byte_progress(static_cast<std::uintmax_t>(compressed_bytes_read), archive_size, 0, 30);
                }
            }
            archive_status = archive_write_finish_entry(archive_writer.get());
            if (archive_status != ARCHIVE_OK) {
                log_archive_error(archive_writer.get(), "Failed to finish archive entry", archive_status);
                return false;
            }
        }

        bool extraction_complete = true;
        archive_status = archive_read_close(archive_reader.get());
        if (archive_status != ARCHIVE_OK) {
            log_archive_error(archive_reader.get(), "Failed to close archive reader", archive_status);
            extraction_complete = false;
        }
        archive_status = archive_write_close(archive_writer.get());
        if (archive_status != ARCHIVE_OK) {
            log_archive_error(archive_writer.get(), "Failed to close archive writer", archive_status);
            extraction_complete = false;
        }
        if (!extraction_complete) {
            return false;
        }
        status_reporter.set_percentage(30);
        std::cout << FlashConfig::LOG_TAG << "Successfully unpacked archive: " << archive_path << " to directory: " << destination_directory << std::endl;
        return true;
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
        int image_file_descriptor = ::open(image_path.c_str(), O_RDONLY);
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
        struct loop_info64 loop_info {};
        loop_info.lo_flags = LO_FLAGS_PARTSCAN;
        if (::ioctl(m_loop_device_file_descriptor, LOOP_SET_STATUS64, &loop_info) < 0) {
            const int error_number = errno;
            std::cerr << FlashConfig::LOG_TAG << "Failed to enable loop device partition scanning: "
                      << m_loop_device_path << ": " << std::strerror(error_number) << std::endl;
            return false;
        }
        return true;
    }

    /**
     * @brief Determine the passive root filesystem device based on the current active device.
     * This function reads the command line from /proc/cmdline to identify the active root filesystem device.
     * @return The path to the passive root filesystem device.
     */
    std::string get_passive_rootfs_device()
    {
        std::ifstream proc_command_line(FlashConfig::PROC_COMMAND_LINE_PATH);
        std::string command_line;
        if (proc_command_line && std::getline(proc_command_line, command_line) &&
            command_line.find(FlashConfig::ROOTFS_BANK_A_DEVICE) != std::string::npos) {
            return FlashConfig::ROOTFS_BANK_B_DEVICE;
        }
        return FlashConfig::ROOTFS_BANK_A_DEVICE;
    }

    std::uintmax_t get_tree_size(const fs::path& source_path)
    {
        if (fs::is_directory(source_path)) {
            std::uintmax_t total_bytes = 0;
            for (const auto& directory_entry : fs::directory_iterator(source_path)) {
                total_bytes += get_tree_size(directory_entry.path());
            }
            return total_bytes;
        }
        return fs::is_regular_file(source_path) ? fs::file_size(source_path) : 0;
    }

    void copy_file_with_progress(const fs::path& source_path, const fs::path& destination_path,
                                 const std::function<void(std::uintmax_t)>& report_copied_bytes)
    {
        if (fs::exists(destination_path) && fs::equivalent(source_path, destination_path)) {
            throw fs::filesystem_error("Copy source and destination are the same file", source_path, destination_path,
                                       std::make_error_code(std::errc::file_exists));
        }
        if (fs::exists(destination_path) && !fs::is_regular_file(destination_path)) {
            throw fs::filesystem_error("Copy destination is not a regular file", source_path, destination_path,
                                       std::make_error_code(std::errc::invalid_argument));
        }
        std::ifstream source_stream(source_path, std::ios::binary);
        if (!source_stream) {
            throw fs::filesystem_error("Failed to open copy source", source_path, destination_path,
                                       std::make_error_code(std::errc::io_error));
        }
        std::ofstream destination_stream(destination_path, std::ios::binary | std::ios::trunc);
        if (!destination_stream) {
            throw fs::filesystem_error("Failed to open copy destination", source_path, destination_path,
                                       std::make_error_code(std::errc::io_error));
        }
        std::array<char, FlashConfig::FILE_COPY_BLOCK_SIZE> buffer;
        while (source_stream) {
            source_stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            if (source_stream.bad() || (source_stream.fail() && !source_stream.eof())) {
                throw fs::filesystem_error("Failed to read copy source", source_path, destination_path,
                                           std::make_error_code(std::errc::io_error));
            }
            const auto copied_bytes = source_stream.gcount();
            if (copied_bytes > 0) {
                destination_stream.write(buffer.data(), copied_bytes);
                if (!destination_stream) {
                    throw fs::filesystem_error("Failed to write copy destination", source_path, destination_path,
                                               std::make_error_code(std::errc::io_error));
                }
                report_copied_bytes(static_cast<std::uintmax_t>(copied_bytes));
            }
        }
        destination_stream.close();
        if (!destination_stream) {
            throw fs::filesystem_error("Failed to close copy destination", source_path, destination_path,
                                       std::make_error_code(std::errc::io_error));
        }
        fs::permissions(destination_path, fs::status(source_path).permissions());
    }

    /**
     * @brief Recursively copy the contents of a source directory to a destination directory.
     * If the source is a file, it will be copied directly to the destination.
     * @param source_path The path to the source directory or file.
     * @param destination_path The path to the destination directory.
     * @param report_copied_bytes Optional callback receiving each copied block's byte count.
     */
    void copy_tree(const fs::path& source_path, const fs::path& destination_path,
                   const std::function<void(std::uintmax_t)>& report_copied_bytes = {})
    {
        if (fs::is_directory(source_path)) {
            fs::create_directories(destination_path);
            for (auto& directory_entry : fs::directory_iterator(source_path)) {
                copy_tree(directory_entry.path(), destination_path / directory_entry.path().filename(), report_copied_bytes);
            }
        } else if (fs::is_regular_file(source_path)) {
            if (report_copied_bytes) {
                copy_file_with_progress(source_path, destination_path, report_copied_bytes);
            } else {
                fs::copy_file(source_path, destination_path, fs::copy_options::overwrite_existing);
            }
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
        ::sync();
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

        size_t replacement_count = 0;
        for (size_t device_position = command_line_contents.find(active_rootfs_device);
             device_position != std::string::npos;
             device_position = command_line_contents.find(active_rootfs_device, device_position + passive_rootfs_device.length())) {
            command_line_contents.replace(device_position, active_rootfs_device.length(), passive_rootfs_device);
            ++replacement_count;
        }
        if (replacement_count == 0) {
            if (command_line_contents.find(passive_rootfs_device) != std::string::npos) {
                std::cout << FlashConfig::LOG_TAG << "Boot command line already uses " << passive_rootfs_device << "." << std::endl;
                return true;
            }
            std::cerr << FlashConfig::LOG_TAG << "Boot command line references neither " << active_rootfs_device
                      << " nor " << passive_rootfs_device << "." << std::endl;
            return false;
        }

        std::ofstream command_line_output(FlashConfig::BOOT_COMMAND_LINE_PATH, std::ios::trunc);
        command_line_output << command_line_contents;
        command_line_output.close();
        if (!command_line_output) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to write boot command line: " << FlashConfig::BOOT_COMMAND_LINE_PATH << std::endl;
            return false;
        }
        ::sync();
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
        const auto fail = [&failure_reason](mfrError_t error, const char* reason) {
            failure_reason = reason;
            return error;
        };

        // Apply CPU, IO, and core affinity constraints natively before doing heavy lifting
        apply_reduced_priority();
        status_reporter.set_phase(mfrUPGRADE_PROGRESS_VERIFYING, 0);

        if (is_gzip_archive(m_image_path)) {
            std::cout << FlashConfig::LOG_TAG << "Image file is a GZIP archive. Unpacking..." << std::endl;
            fs::create_directories(FlashConfig::EXTRACTED_IMAGE_DIRECTORY);
            if (!unpack_archive(m_image_path, FlashConfig::EXTRACTED_IMAGE_DIRECTORY, status_reporter)) {
                std::cerr << FlashConfig::LOG_TAG << "Failed to unpack GZIP archive: " << m_image_path << std::endl;
                return fail(mfrERR_SRC_FILE_ERROR, "Archive unpack failed");
            }
            std::string extracted_disk_image_path;
            for (auto& directory_entry : fs::directory_iterator(FlashConfig::EXTRACTED_IMAGE_DIRECTORY)) {
                if (directory_entry.is_regular_file() && has_mbr_signature(directory_entry.path().string())) {
                    extracted_disk_image_path = directory_entry.path().string();
                    break;
                }
            }
            if (extracted_disk_image_path.empty()) {
                std::cerr << FlashConfig::LOG_TAG << "No valid disk image with MBR signature found in unpacked archive." << std::endl;
                return fail(mfrERR_BAD_IMAGE_HEADER, "No MBR disk image in archive");
            }
            fs::remove(m_image_path);
            m_image_path = extracted_disk_image_path;
        } else if (!has_mbr_signature(m_image_path)) {
            return fail(mfrERR_BAD_IMAGE_HEADER, "Image has no MBR signature");
        }
        status_reporter.set_percentage(35);

        fs::create_directories(FlashConfig::BOOT_MOUNT_DIRECTORY);
        fs::create_directories(FlashConfig::ROOT_FILESYSTEM_MOUNT_DIRECTORY);
        fs::create_directories(FlashConfig::TARGET_ROOT_FILESYSTEM_MOUNT_DIRECTORY);
        fs::remove_all(FlashConfig::BOOT_BACKUP_DIRECTORY);
        fs::create_directories(FlashConfig::BOOT_BACKUP_DIRECTORY);
        copy_tree_with_progress(FlashConfig::BOOT_DIRECTORY, FlashConfig::BOOT_BACKUP_DIRECTORY, status_reporter, 35, 40);
        if (!setup_loop_device(m_image_path)) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to set up loop device for image: " << m_image_path << std::endl;
            return fail(mfrERR_SRC_FILE_ERROR, "Loop device setup failed");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(FlashConfig::LOOP_DEVICE_STABILIZATION_DELAY_MS));
        status_reporter.set_percentage(40);

        m_is_critical_update_phase.store(true, std::memory_order_release);

        std::string image_rootfs_partition = m_loop_device_path + "p2";
        if (::mount(image_rootfs_partition.c_str(),
                    FlashConfig::ROOT_FILESYSTEM_MOUNT_DIRECTORY,
                    FlashConfig::EXT4_FILESYSTEM_TYPE, MS_RDONLY, nullptr) != 0) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to mount image root filesystem partition: " << image_rootfs_partition
                      << ": " << std::strerror(errno) << std::endl;
            return fail(mfrERR_BAD_IMAGE_HEADER, "Image rootfs mount failed");
        }
        std::string passive_rootfs_device = get_passive_rootfs_device();
        std::cout << FlashConfig::LOG_TAG << "Passive root filesystem determined: " << passive_rootfs_device << std::endl;
        if (::mount(passive_rootfs_device.c_str(),
                    FlashConfig::TARGET_ROOT_FILESYSTEM_MOUNT_DIRECTORY,
                    FlashConfig::EXT4_FILESYSTEM_TYPE, 0, nullptr) != 0) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to mount passive root filesystem device: " << passive_rootfs_device
                      << ": " << std::strerror(errno) << std::endl;
            return fail(mfrERR_WRITE_FLASH_FAILED, "Passive rootfs mount failed");
        }
        status_reporter.set_phase(mfrUPGRADE_PROGRESS_FLASHING, 45);
        for (auto& directory_entry : fs::directory_iterator(FlashConfig::TARGET_ROOT_FILESYSTEM_MOUNT_DIRECTORY)) {
            fs::remove_all(directory_entry.path());
        }
        copy_tree_with_progress(FlashConfig::ROOT_FILESYSTEM_MOUNT_DIRECTORY, FlashConfig::TARGET_ROOT_FILESYSTEM_MOUNT_DIRECTORY,
                                status_reporter, 45, 80);
        ::sync();
        ::umount(FlashConfig::TARGET_ROOT_FILESYSTEM_MOUNT_DIRECTORY);
        ::umount(FlashConfig::ROOT_FILESYSTEM_MOUNT_DIRECTORY);
        std::cout << FlashConfig::LOG_TAG << "Root filesystem copied to " << passive_rootfs_device << "." << std::endl;
        status_reporter.set_percentage(80);

        // Like FlashApp.sh, only switch banks when both the boot and root filesystem updates succeed.
        std::string image_boot_partition = m_loop_device_path + "p1";
        if (::mount(image_boot_partition.c_str(),
                    FlashConfig::BOOT_MOUNT_DIRECTORY,
                    FlashConfig::VFAT_FILESYSTEM_TYPE, MS_RDONLY, nullptr) != 0) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to mount image boot partition: " << image_boot_partition
                      << ": " << std::strerror(errno) << std::endl;
            return fail(mfrERR_BAD_IMAGE_HEADER, "Image boot mount failed");
        }
        try {
            replace_boot_contents(FlashConfig::BOOT_MOUNT_DIRECTORY, &status_reporter);
        } catch (const std::exception& exception) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to update " << FlashConfig::BOOT_DIRECTORY << ": " << exception.what() << std::endl;
            ::umount(FlashConfig::BOOT_MOUNT_DIRECTORY);
            restore_boot_backup();
            return fail(mfrERR_WRITE_FLASH_FAILED, "Boot partition update failed");
        }
        ::umount(FlashConfig::BOOT_MOUNT_DIRECTORY);
        std::cout << FlashConfig::LOG_TAG << "Boot partition contents updated." << std::endl;
        status_reporter.set_percentage(90);

        std::string active_rootfs_device = (passive_rootfs_device == FlashConfig::ROOTFS_BANK_A_DEVICE)
            ? FlashConfig::ROOTFS_BANK_B_DEVICE : FlashConfig::ROOTFS_BANK_A_DEVICE;
        if (!update_boot_command_line(active_rootfs_device, passive_rootfs_device)) {
            restore_boot_backup();
            return fail(mfrERR_UPDATE_BOOT_PARAMS_FAILED, "Boot cmdline update failed");
        }
        status_reporter.set_percentage(99);
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
     * @brief Perform the flashing process.
     * This function handles the entire flashing process, including unpacking the image if it's a GZIP archive,
     * setting up loop devices, mounting partitions, copying files, and updating the boot command line.
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
        ::umount(FlashConfig::TARGET_ROOT_FILESYSTEM_MOUNT_DIRECTORY);
        ::umount(FlashConfig::ROOT_FILESYSTEM_MOUNT_DIRECTORY);
        ::umount(FlashConfig::BOOT_MOUNT_DIRECTORY);
        if (m_loop_device_file_descriptor >= 0) {
            ::ioctl(m_loop_device_file_descriptor, LOOP_CLR_FD, 0);
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
            std::error_code remove_error;
            fs::remove(FlashConfig::INSTANCE_LOCK_FILE, remove_error);
            if (remove_error) {
                std::cerr << FlashConfig::LOG_TAG << "Failed to remove lock file: " << remove_error.message() << std::endl;
            }
        }
        std::error_code remove_error;
        fs::remove_all(FlashConfig::TEMPORARY_MOUNT_ROOT, remove_error);
        if (remove_error) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to remove temporary mount directory: " << remove_error.message() << std::endl;
        }
        remove_error.clear();
        fs::remove_all(FlashConfig::EXTRACTED_IMAGE_DIRECTORY, remove_error);
        if (remove_error) {
            std::cerr << FlashConfig::LOG_TAG << "Failed to remove extracted image directory: " << remove_error.message() << std::endl;
        }
        ::sync();
    }
};
