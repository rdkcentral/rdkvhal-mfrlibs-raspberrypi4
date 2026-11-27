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
#include <memory>

std::unique_ptr<NativeFlashProcessor> flash_processor = nullptr;

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
    std::string ota_image_path;

    if (argument_count == 3) {
        ota_image_path = (fs::path(argument_values[1]) / argument_values[2]).string();
    } else if (argument_count == 2) {
        ota_image_path = argument_values[1];
    } else {
        std::cerr << "Usage Method 1: " << argument_values[0] << " <directory path> <ota file name>\n"
                  << "Usage Method 2: " << argument_values[0] << " <absolute path to ota file>" << std::endl;
        return 1;
    }

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGHUP,  signal_handler);

    flash_processor = std::make_unique<NativeFlashProcessor>(ota_image_path);
    if (flash_processor->flash()) {
        return 0;
    }
    return 1;
}
