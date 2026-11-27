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

#include "FlashProcessor.hpp"
#include <csignal>
#include <charconv>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "mfrTypes.h"

std::unique_ptr<NativeFlashProcessor> flash_processor;

constexpr std::string_view upgrade_progress_name(mfrUpgradeProgress_t progress) noexcept
{
    switch (progress) {
        case mfrUPGRADE_PROGRESS_NOT_STARTED: return "NOT_STARTED";
        case mfrUPGRADE_PROGRESS_STARTED:     return "STARTED";
        case mfrUPGRADE_PROGRESS_ABORTED:     return "ABORTED";
        case mfrUPGRADE_PROGRESS_VERIFYING:   return "VERIFYING";
        case mfrUPGRADE_PROGRESS_FLASHING:    return "FLASHING";
        case mfrUPGRADE_PROGRESS_REBOOTING:   return "REBOOTING";
        case mfrUPGRADE_PROGRESS_COMPLETED:   return "COMPLETED";
        default:                              return "UNKNOWN";
    }
}

bool parse_notification_interval(std::string_view value, int& interval) noexcept
{
    if (value.empty()) {
        return false;
    }
    int parsed_interval = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed_interval);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed_interval < 0) {
        return false;
    }
    interval = parsed_interval;
    return true;
}

/**
 * @brief Signal handler for the flashing process.
 * This function is invoked when the process receives a signal (e.g., SIGINT, SIGTERM, SIGHUP).
 * It delegates the signal handling to the NativeFlashProcessor instance if it exists.
 * @param signal_number The signal number received by the process.
 */
void signal_handler(int signal_number)
{
    if (flash_processor) flash_processor->handle_signal(signal_number);
    else std::exit(signal_number);
}

/**
 * @brief Main entry point for the flashing application.
 * This function processes command-line arguments, sets up signal handling, and initiates the flashing process.
 * @param argument_count The number of command-line arguments.
 * @param argument_values The array of command-line argument strings.
 * @return Exit status code (0 for success, 1 for failure).
 */
int main(int argument_count, char* argument_values[])
{
#if defined(GIT_SHA)
    std::cout << FlashConfig::LOG_TAG << "Version: " << GIT_SHA << std::endl;
#endif
    std::string ota_image_path;

    if (argument_count == 3 || argument_count == 4) {
        ota_image_path = (std::filesystem::path(argument_values[1]) / argument_values[2]).string();
    } else if (argument_count == 2) {
        ota_image_path = argument_values[1];
    } else {
        std::cerr << "Usage Method 1: " << argument_values[0] << " <directory path> <ota file name> <notification interval in sec>\n"
                  << "Usage Method 2: " << argument_values[0] << " <directory path> <ota file name>\n"
                  << "Usage Method 3: " << argument_values[0] << " <absolute path to ota file>" << std::endl;
        return 1;
    }

    int notify_interval_seconds = FlashConfig::STATUS_NOTIFY_INTERVAL_SECONDS;
    if (argument_count == 4) {
        if (!parse_notification_interval(argument_values[3], notify_interval_seconds)) {
            std::cerr << FlashConfig::LOG_TAG << "Notification interval must be a non-negative integer: "
                      << argument_values[3] << std::endl;
            return 1;
        }
    }

    std::cout << FlashConfig::LOG_TAG << "Flashing image: " << ota_image_path
              << " with status notification interval: " << notify_interval_seconds << " seconds." << std::endl;

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGHUP,  signal_handler);

    mfrUpgradeStatusNotify_t notify {};
    notify.interval = notify_interval_seconds;
    notify.cbData = nullptr;
    notify.cb = [](mfrUpgradeStatus_t *status) {
        if (status) {
            std::cout << FlashConfig::LOG_TAG
                      << "Upgrade status: " << std::left << std::setw(16) << upgrade_progress_name(status->progress)
                      << " (" << status->progress << ")"
                      << ", percentage:" << std::right << std::setw(3) << status->percentage << "%"
                      << ", error:" << status->error
                      << ", error string:'" << status->error_string << "'" << std::endl;
        }
    };

    flash_processor = std::make_unique<NativeFlashProcessor>(ota_image_path);
    return (flash_processor->flash(notify) == mfrERR_NONE) ? 0 : 1;
}
