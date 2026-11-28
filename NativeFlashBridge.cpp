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

#include "NativeFlashBridge.h"
#include "FlashProcessor.hpp"
#include <new>

namespace {

thread_local bool native_callback_active = false;

class NativeFlashWorker;
NativeFlashWorker& native_worker();
void native_status_callback(mfrUpgradeStatus_t* status);

void report_status(const mfrUpgradeStatusNotify_t& notify, mfrUpgradeProgress_t progress,
                   mfrError_t error, const char* message)
{
    mfrUpgradeStatus_t status {};
    status.progress = progress;
    status.error = error;
    std::snprintf(status.error_string, sizeof(status.error_string), "%s", message);
    notify.cb(&status);
}

class NativeFlashWorker {
    std::mutex m_mutex;
    std::thread m_worker;
    mfrUpgradeStatusNotify_t m_notify {};
    bool m_busy = false;
    bool m_waiting = false;

public:
    ~NativeFlashWorker()
    {
        wait();
    }

    mfrError_t start(const char* image_path, const mfrUpgradeStatusNotify_t& notify)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_busy || m_waiting) {
            std::cerr << FlashConfig::LOG_TAG << "A native HAL flash is already active." << std::endl;
            return mfrERR_OPERATION_NOT_SUPPORTED;
        }
        if (m_worker.joinable()) {
            m_worker.join();
        }

        auto processor = std::make_unique<NativeFlashProcessor>(image_path);
        const mfrError_t preparation_result = processor->prepare(notify);
        if (preparation_result != mfrERR_NONE) {
            return preparation_result;
        }

        m_notify = notify;
        mfrUpgradeStatusNotify_t worker_notify = notify;
        worker_notify.cb = native_status_callback;
        m_busy = true;
        try {
            m_worker = std::thread([this, processor = std::move(processor), worker_notify]() mutable {
                // Wait until start() has stored m_worker so wait() can recognize this thread.
                // This is not an API-return barrier: callbacks may precede native_flash_start()'s return.
                {
                    std::lock_guard<std::mutex> start_lock(m_mutex);
                }
                try {
                    if (worker_notify.interval > 0) {
                        report_status(worker_notify, mfrUPGRADE_PROGRESS_STARTED, mfrERR_NONE, "");
                    }
                    processor->flash(worker_notify, true);
                } catch (const std::bad_alloc& exception) {
                    std::cerr << FlashConfig::LOG_TAG << "Native HAL worker allocation failed: "
                              << exception.what() << std::endl;
                    report_status(worker_notify, mfrUPGRADE_PROGRESS_ABORTED, mfrERR_MEMORY_EXHAUSTED,
                                  "Native flash worker allocation failed");
                } catch (const std::exception& exception) {
                    std::cerr << FlashConfig::LOG_TAG << "Native HAL worker failed: "
                              << exception.what() << std::endl;
                    report_status(worker_notify, mfrUPGRADE_PROGRESS_ABORTED, mfrERR_WRITE_FLASH_FAILED,
                                  "Native flash worker failed");
                }
                processor.reset();
                std::lock_guard<std::mutex> completion_lock(m_mutex);
                m_busy = false;
            });
        } catch (const std::exception&) {
            m_busy = false;
            throw;
        }
        return mfrERR_NONE;
    }

    mfrError_t wait()
    {
        if (native_callback_active) {
            std::cerr << FlashConfig::LOG_TAG << "Cannot terminate the HAL from a flash callback." << std::endl;
            return mfrERR_OPERATION_NOT_SUPPORTED;
        }
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_waiting || (m_worker.joinable() && m_worker.get_id() == std::this_thread::get_id())) {
            std::cerr << FlashConfig::LOG_TAG << "Cannot terminate the HAL from its flash callback "
                      << "or while termination is already in progress." << std::endl;
            return mfrERR_OPERATION_NOT_SUPPORTED;
        }
        m_waiting = true;
        std::thread worker = std::move(m_worker);
        lock.unlock();
        if (worker.joinable()) {
            worker.join();
        }
        lock.lock();
        m_waiting = false;
        return mfrERR_NONE;
    }

    void deliver(mfrUpgradeStatus_t* status)
    {
        struct CallbackScope {
            const bool previous = native_callback_active;
            CallbackScope() { native_callback_active = true; }
            ~CallbackScope() { native_callback_active = previous; }
        } callback_scope;
        m_notify.cb(status);
    }
};

NativeFlashWorker& native_worker()
{
    static NativeFlashWorker worker;
    return worker;
}

void native_status_callback(mfrUpgradeStatus_t* status)
{
    native_worker().deliver(status);
}

}

extern "C" mfrError_t native_flash_start(const char* image_path, mfrUpgradeStatusNotify_t notify)
{
    if (!image_path || !*image_path || !notify.cb || notify.interval < 0) {
        std::cerr << FlashConfig::LOG_TAG << "Invalid native HAL flash request." << std::endl;
        return mfrERR_INVALID_PARAM;
    }
    try {
        return native_worker().start(image_path, notify);
    } catch (const std::bad_alloc& exception) {
        std::cerr << FlashConfig::LOG_TAG << "Unable to allocate native HAL worker: "
                  << exception.what() << std::endl;
        return mfrERR_MEMORY_EXHAUSTED;
    } catch (const std::exception& exception) {
        std::cerr << FlashConfig::LOG_TAG << "Unable to start native HAL worker: "
                  << exception.what() << std::endl;
        return mfrERR_WRITE_FLASH_FAILED;
    }
}

extern "C" mfrError_t native_flash_wait(void)
{
    try {
        return native_worker().wait();
    } catch (const std::exception& exception) {
        std::cerr << FlashConfig::LOG_TAG << "Unable to wait for native HAL worker: "
                  << exception.what() << std::endl;
        return mfrERR_WRITE_FLASH_FAILED;
    }
}
