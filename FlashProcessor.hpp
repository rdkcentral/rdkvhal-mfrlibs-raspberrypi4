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
#include <chrono>
#include <thread>
#include <atomic>
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

namespace fs = std::filesystem;

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
            return false;
        }
        if (::flock(m_lock_file_descriptor, LOCK_EX | LOCK_NB) < 0) {
            std::cerr << FlashConfig::LOG_TAG << "Instance active. Aborting." << std::endl;
            ::close(m_lock_file_descriptor);
            m_lock_file_descriptor = FlashConfig::INVALID_FILE_DESCRIPTOR;
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
        std::ifstream image_stream(image_path, std::ios::binary);
        if (!image_stream) {
            return false;
        }
        image_stream.seekg(FlashConfig::MBR_SIGNATURE_OFFSET, std::ios::beg);
        unsigned char mbr_signature[FlashConfig::MBR_SIGNATURE_SIZE] = {0};
        image_stream.read(reinterpret_cast<char*>(mbr_signature), FlashConfig::MBR_SIGNATURE_SIZE);
        return (image_stream.gcount() == FlashConfig::MBR_SIGNATURE_SIZE &&
                mbr_signature[0] == FlashConfig::MBR_SIGNATURE_BYTE_1 &&
                mbr_signature[1] == FlashConfig::MBR_SIGNATURE_BYTE_2);
    }

    /**
     * @brief Unpack a GZIP archive to the specified destination directory using libarchive.
     * @param archive_path The path to the GZIP archive.
     * @param destination_directory The directory where the archive contents will be extracted.
     * @return true if the extraction was successful, false otherwise.
     */
    bool unpack_archive(const std::string& archive_path, const std::string& destination_directory)
    {
        struct archive* archive_reader = archive_read_new();
        struct archive* archive_writer = archive_write_disk_new();
        archive_write_disk_set_options(archive_writer, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM);
        archive_read_support_filter_gzip(archive_reader); archive_read_support_format_tar(archive_reader);
        if (archive_read_open_filename(archive_reader, archive_path.c_str(), FlashConfig::ARCHIVE_READ_BLOCK_SIZE) != ARCHIVE_OK) {
            archive_read_free(archive_reader);
            archive_write_free(archive_writer);
            return false;
        }
        struct archive_entry* archive_entry;
        int archive_status = ARCHIVE_OK;
        while (archive_read_next_header(archive_reader, &archive_entry) == ARCHIVE_OK) {
            fs::path extracted_entry_path = fs::path(destination_directory) / archive_entry_pathname(archive_entry);
            archive_entry_set_pathname(archive_entry, extracted_entry_path.c_str());
            archive_status = archive_write_header(archive_writer, archive_entry);
            if (archive_status >= ARCHIVE_OK && archive_entry_size(archive_entry) > 0) {
                const void* data_block;
                size_t data_block_size;
                la_int64_t data_block_offset;
                while (archive_read_data_block(archive_reader, &data_block, &data_block_size, &data_block_offset) == ARCHIVE_OK) {
                    if (archive_write_data_block(archive_writer, data_block, data_block_size, data_block_offset) != ARCHIVE_OK) break;
                }
            }
            archive_write_finish_entry(archive_writer);
        }
        archive_read_close(archive_reader); archive_read_free(archive_reader);
        archive_write_close(archive_writer); archive_write_free(archive_writer);
        return (archive_status == ARCHIVE_OK);
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
            return false;
        }
        int loop_device_number = ::ioctl(m_loop_control_file_descriptor, LOOP_CTL_GET_FREE);
        if (loop_device_number < 0) {
            return false;
        }
        m_loop_device_path = std::string(FlashConfig::LOOP_DEVICE_PREFIX) + std::to_string(loop_device_number);
        m_loop_device_file_descriptor = ::open(m_loop_device_path.c_str(), O_RDWR);
        if (m_loop_device_file_descriptor < 0) {
            return false;
        }
        int image_file_descriptor = ::open(image_path.c_str(), O_RDONLY);
        if (image_file_descriptor < 0) {
            return false;
        }
        if (::ioctl(m_loop_device_file_descriptor, LOOP_SET_FD, image_file_descriptor) < 0) {
            ::close(image_file_descriptor);
            return false;
        }
        ::close(image_file_descriptor);
        return (::ioctl(m_loop_device_file_descriptor, LOOP_SET_STATUS64, nullptr) >= 0);
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

    /**
     * @brief Recursively copy the contents of a source directory to a destination directory.
     * If the source is a file, it will be copied directly to the destination.
     * @param source_path The path to the source directory or file.
     * @param destination_path The path to the destination directory.
     */
    void copy_tree(const fs::path& source_path, const fs::path& destination_path)
    {
        if (fs::is_directory(source_path)) {
            fs::create_directories(destination_path);
            for (auto& directory_entry : fs::directory_iterator(source_path)) {
                copy_tree(directory_entry.path(), destination_path / directory_entry.path().filename());
            }
        } else if (fs::is_regular_file(source_path)) {
            fs::copy_file(source_path, destination_path, fs::copy_options::overwrite_existing);
        }
    }

public:
    explicit NativeFlashProcessor(const std::string& image_path) : m_image_path(image_path) {}
    ~NativeFlashProcessor() { cleanup(); }

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
            cleanup(); std::exit(signal_number);
        }
    }

    /**
     * @brief Perform the flashing process.
     * This function handles the entire flashing process, including unpacking the image if it's a GZIP archive,
     * setting up loop devices, mounting partitions, copying files, and updating the boot command line.
     * @return true if the flashing process was successful, false otherwise.
     */
    bool flash()
    {
        if (!lock_instance() || !fs::exists(m_image_path)) {
            return false;
        }

        // Apply CPU, IO, and core affinity constraints natively before doing heavy lifting
        apply_reduced_priority();

        if (is_gzip_archive(m_image_path)) {
            fs::create_directories(FlashConfig::EXTRACTED_IMAGE_DIRECTORY);
            if (!unpack_archive(m_image_path, FlashConfig::EXTRACTED_IMAGE_DIRECTORY)) {
                return false;
            }
            std::string extracted_disk_image_path;
            for (auto& directory_entry : fs::directory_iterator(FlashConfig::EXTRACTED_IMAGE_DIRECTORY)) {
                if (directory_entry.is_regular_file() && has_mbr_signature(directory_entry.path().string())) {
                    extracted_disk_image_path = directory_entry.path().string();
                    break;
                }
            }
            if (extracted_disk_image_path.empty()) {
                return false;
            }
            fs::remove(m_image_path);
            m_image_path = extracted_disk_image_path;
        }

        fs::create_directories(FlashConfig::BOOT_MOUNT_DIRECTORY);
        fs::create_directories(FlashConfig::ROOT_FILESYSTEM_MOUNT_DIRECTORY);
        fs::create_directories(FlashConfig::TARGET_ROOT_FILESYSTEM_MOUNT_DIRECTORY);
        fs::create_directories(FlashConfig::BOOT_BACKUP_DIRECTORY);
        fs::remove_all(FlashConfig::BOOT_BACKUP_DIRECTORY);
        copy_tree(FlashConfig::BOOT_DIRECTORY, FlashConfig::BOOT_BACKUP_DIRECTORY);
        if (!setup_loop_device(m_image_path)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(FlashConfig::LOOP_DEVICE_STABILIZATION_DELAY_MS));

        m_is_critical_update_phase.store(true, std::memory_order_release);

        std::string image_rootfs_partition = m_loop_device_path + "p2";
        if (::mount(image_rootfs_partition.c_str(),
                    FlashConfig::ROOT_FILESYSTEM_MOUNT_DIRECTORY,
                    FlashConfig::EXT4_FILESYSTEM_TYPE, MS_RDONLY, nullptr) != 0) {
            m_is_critical_update_phase.store(false, std::memory_order_release); {
                return false;
            }
        }
        std::string passive_rootfs_device = get_passive_rootfs_device();
        if (::mount(passive_rootfs_device.c_str(),
                    FlashConfig::TARGET_ROOT_FILESYSTEM_MOUNT_DIRECTORY,
                    FlashConfig::EXT4_FILESYSTEM_TYPE, 0, nullptr) != 0) {
            ::umount(FlashConfig::ROOT_FILESYSTEM_MOUNT_DIRECTORY);
            m_is_critical_update_phase.store(false, std::memory_order_release);
            return false;
        }
        for (auto& directory_entry : fs::directory_iterator(FlashConfig::TARGET_ROOT_FILESYSTEM_MOUNT_DIRECTORY)) {
            fs::remove_all(directory_entry.path());
        }
        copy_tree(FlashConfig::ROOT_FILESYSTEM_MOUNT_DIRECTORY, FlashConfig::TARGET_ROOT_FILESYSTEM_MOUNT_DIRECTORY);
        ::umount(FlashConfig::TARGET_ROOT_FILESYSTEM_MOUNT_DIRECTORY);
        ::umount(FlashConfig::ROOT_FILESYSTEM_MOUNT_DIRECTORY);

        std::string image_boot_partition = m_loop_device_path + "p1";
        if (::mount(image_boot_partition.c_str(),
                    FlashConfig::BOOT_MOUNT_DIRECTORY,
                    FlashConfig::VFAT_FILESYSTEM_TYPE, MS_RDONLY, nullptr) == 0) {
            for (auto& directory_entry : fs::directory_iterator(FlashConfig::BOOT_DIRECTORY)) {
                fs::remove_all(directory_entry.path());
            }
            copy_tree(FlashConfig::BOOT_MOUNT_DIRECTORY, FlashConfig::BOOT_DIRECTORY);
            ::umount(FlashConfig::BOOT_MOUNT_DIRECTORY);
        }
        std::string active_rootfs_device = (passive_rootfs_device == FlashConfig::ROOTFS_BANK_A_DEVICE)
            ? FlashConfig::ROOTFS_BANK_B_DEVICE : FlashConfig::ROOTFS_BANK_A_DEVICE;
        std::ifstream command_line_input(FlashConfig::BOOT_COMMAND_LINE_PATH);
        if (command_line_input) {
            std::string command_line_contents((std::istreambuf_iterator<char>(command_line_input)),
                                              std::istreambuf_iterator<char>());
            command_line_input.close();
            size_t active_device_position = command_line_contents.find(active_rootfs_device);
            if (active_device_position != std::string::npos) {
                command_line_contents.replace(active_device_position, active_rootfs_device.length(), passive_rootfs_device);
                std::ofstream command_line_output(FlashConfig::BOOT_COMMAND_LINE_PATH, std::ios::trunc);
                command_line_output << command_line_contents;
            }
        }

        m_is_critical_update_phase.store(false, std::memory_order_release);

        if (m_has_deferred_signal.load(std::memory_order_acquire)) {
            std::cout << FlashConfig::LOG_TAG << "Processing deferred signal (" << m_deferred_signal_number.load() << ")." << std::endl;
            cleanup();
            std::exit(m_deferred_signal_number.load());
        }
        return true;
    }

    /**
     * @brief Perform cleanup operations, including unmounting directories, releasing loop devices,
     * closing file descriptors, and removing temporary files and directories.
     */
    void cleanup()
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
            fs::remove(FlashConfig::INSTANCE_LOCK_FILE);
        }
        fs::remove_all(FlashConfig::TEMPORARY_MOUNT_ROOT);
        fs::remove_all(FlashConfig::EXTRACTED_IMAGE_DIRECTORY);
        ::sync();
    }
};
