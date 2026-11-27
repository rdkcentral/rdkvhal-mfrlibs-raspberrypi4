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
#include <cstddef>

namespace FlashConfig {
    static constexpr const char* TEMPORARY_MOUNT_ROOT = "/opt/ota/extblock";
    static constexpr const char* BOOT_MOUNT_DIRECTORY = "/opt/ota/extblock/ota_boot";
    static constexpr const char* BOOT_BACKUP_DIRECTORY = "/opt/ota/extblock/old_boot_bkup";
    static constexpr const char* BOOT_DIRECTORY = "/boot";
    static constexpr const char* BOOT_COMMAND_LINE_PATH = "/boot/cmdline.txt";
    static constexpr const char* PROC_COMMAND_LINE_PATH = "/proc/cmdline";
    static constexpr const char* LOOP_CONTROL_DEVICE = "/dev/loop-control";
    static constexpr const char* LOOP_DEVICE_PREFIX = "/dev/loop";
    static constexpr const char* INSTANCE_LOCK_FILE = "/var/run/flash_app.lock";
    static constexpr const char* VFAT_FILESYSTEM_TYPE = "vfat";
    static constexpr const char* LOG_TAG = "[FLASHER] ";
    static constexpr const char* ROOTFS_BANK_A_DEVICE = "/dev/mmcblk0p2";
    static constexpr const char* ROOTFS_BANK_B_DEVICE = "/dev/mmcblk0p3";
    static constexpr std::size_t GZIP_HEADER_SIZE = 2;
    static constexpr unsigned char GZIP_HEADER_BYTE_1 = 0x1F;
    static constexpr unsigned char GZIP_HEADER_BYTE_2 = 0x8B;
    static constexpr int INVALID_FILE_DESCRIPTOR = -1;
    static constexpr int LOOP_DEVICE_STABILIZATION_DELAY_MS = 50;
    static constexpr int STATUS_NOTIFY_INTERVAL_SECONDS = 2;

    // Direct Native Resource Throttling Parameters
    static constexpr int CPU_NICE_LOW_PRIORITY = 10;
    static constexpr int IO_PRIORITY_PROCESS   = 1;
    static constexpr int IO_PRIORITY_CLASS_IDLE = 3;
    static constexpr int IO_PRIORITY_INFO_SHIFT = 13;
    static constexpr int CPU_AFFINITY_CORE_2   = 2;
    static constexpr int CPU_AFFINITY_CORE_3   = 3;
}
