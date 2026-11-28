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

#include <cstdint>
#include <functional>
#include <string>

struct WicStreamResult {
    bool success = false;
    int error_code = 0;
    std::string message;
    std::string boot_image_path;
    std::uint64_t bytes_written = 0;
    std::uint64_t total_bytes = 0;
};

using WicStreamProgress = std::function<void(std::uint64_t, std::uint64_t)>;
using WicStreamWriteStart = std::function<void()>;

/**
 * @brief Stream a .wic image to the boot and rootfs partitions of a Raspberry Pi 4.
 * @param image_path Path to the .wic image file.
 * @param rootfs_device Path to the rootfs partition block device (e.g., "/dev/mmcblk0p3").
 * @param progress Optional callback to report progress; receives bytes written and total bytes.
 * @param on_write_start Optional callback invoked once immediately before the first partition write,
 * after initial image, partition-table, and target-capacity checks succeed.
 * @return WicStreamResult indicating success or failure, with details.
 */
WicStreamResult stream_wic_image(const std::string& image_path,
                                 const std::string& rootfs_device,
                                 const WicStreamProgress& progress = {},
                                 const WicStreamWriteStart& on_write_start = {});

/**
 * @brief Unmount every mount point of a configured rootfs bank so it can be opened exclusively.
 * Refuses to touch the running root filesystem and never uses a lazy (detached) unmount.
 * Matches the device by block-device identity, not by its path string.
 * @return true if the device is no longer mounted; otherwise error and error_code describe why.
 */
bool unmount_rootfs_bank(const std::string& device_path, std::string& error, int& error_code);
