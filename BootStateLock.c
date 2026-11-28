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

#define _GNU_SOURCE
#include "BootStateLock.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

int boot_state_lock(void)
{
    int descriptor = open(BOOT_STATE_LOCK_FILE, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
                          S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        return -1;
    }
    struct stat status;
    int stat_result = fstat(descriptor, &status);
    if (stat_result != 0 || !S_ISREG(status.st_mode)) {
        int error = stat_result != 0 ? errno : EINVAL;
        close(descriptor);
        errno = error;
        return -1;
    }
    int result;
    do {
        result = flock(descriptor, LOCK_EX | LOCK_NB);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        int error = errno;
        close(descriptor);
        errno = error;
        return -1;
    }
    return descriptor;
}

int boot_state_unlock(int descriptor)
{
    int result = flock(descriptor, LOCK_UN);
    int error = result == 0 ? 0 : errno;
    if (close(descriptor) != 0 && error == 0) {
        error = errno;
    }
    if (error != 0) {
        errno = error;
        return -1;
    }
    return 0;
}
