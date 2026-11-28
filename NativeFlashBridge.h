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

#ifndef NATIVE_FLASH_BRIDGE_H
#define NATIVE_FLASH_BRIDGE_H

#include <mfrTypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An error rejects the request without callbacks. Success accepts asynchronous work. */
/* Worker callbacks may run during this call; initialize caller state beforehand. */
mfrError_t native_flash_start(const char *image_path, mfrUpgradeStatusNotify_t notify);

/* Wait for the worker and all callbacks before releasing HAL caller resources. */
mfrError_t native_flash_wait(void);

#ifdef __cplusplus
}
#endif

#endif
