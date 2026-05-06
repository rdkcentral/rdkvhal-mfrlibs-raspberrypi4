/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2017 RDK Management
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
*/

#define _GNU_SOURCE
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <mfrMgr.h>
#include <mfrTypes.h>
#include <mfr_wifi_types.h>
#include <mfr_wifi_api.h>

#define MAX_BUF_LEN 255
#define MAC_ADDRESS_SIZE 32
#define LOG_CONFIG_FILE "/etc/debug.ini"

#define BOOT_CONFIG_FILE "/boot/config.txt"
#define BOOT_CONFIG_BACKUP_FILE "/boot/config.txt.bak"

const char defaultDescription[] = "RaspberryPi RDKV Reference Device";
const char defaultProductClass[] = "RDKV";
const char defaultSoftwareVersion[] = "2.0";
static int isInitialized = 0;
static int isDebugEnabled = 0;

/* Mechanism to make this a single instance */
#ifdef ENABLE_SINGLE_INSTANCE_LOCK

#define MFRHAL_LOCK_FILE "/run/mfrhallibrary.lock"
static int lockFd = -1;
static int lockRefCount = 0;

int acquireLock(void)
{
    if (lockRefCount > 0) {
        lockRefCount++;
        return 0;
    }

    int fd = open(MFRHAL_LOCK_FILE, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        return -1;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) == -1) {
        close(fd);
        return -1;
    }

    lockFd = fd;
    lockRefCount = 1;
    return 0;
}

int releaseLock(void)
{
    if (lockRefCount > 1) {
        lockRefCount--;
        return 0;
    }

    if (lockFd != -1) {
        flock(lockFd, LOCK_UN);
        close(lockFd);
        lockFd = -1;
        lockRefCount = 0;
        return 0;
    }
    return -1;
}

#endif /* ENABLE_SINGLE_INSTANCE_LOCK */

int isLibraryInitialized(void)
{
    if (!isInitialized) {
        return 0;
    }

#ifdef ENABLE_SINGLE_INSTANCE_LOCK
    if (lockFd == -1) {
        return 0;
    }
#endif

    return 1;
}

/* Logging function */
void mfrlib_log(const char *format, ...)
{
    if (!isDebugEnabled) {
        return;
    }

    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
}

/**
 * @brief enable/disable debug logging
 * @info This function reads the debug.ini configuration file and enables logging if debug is enabled
 */
void configMFRLibLogging(void)
{
    if (access(LOG_CONFIG_FILE, F_OK) == -1) {
        perror("configMFRLibLogging error accessing debug.ini\n");
        return;
    }
    FILE *file = fopen(LOG_CONFIG_FILE, "r");
    if (!file) {
        perror("configMFRLibLogging error fopen debug.ini\n");
        return;
    }

    char line[MAX_BUF_LEN] = {0};
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\n")] = '\0';
        // Check for the LOG.RDK.MFRMGR entry
        if (strncmp(line, "LOG.RDK.MFRMGR", 14) == 0) {
            if (strstr(line, "DEBUG") && !strstr(line, "!DEBUG")) {
                isDebugEnabled = 1;
            } else {
                isDebugEnabled = 0;
            }
            break;
        }
    }

    fclose(file);
}

/* MFR wrapper implementations */

/**
 * @brief Get the value matching the given key from the version file
 * @param key key to search for in the '/version.txt' file
 * @param separator separator character between key and value
 * @param valueOut output buffer to store the value matching the key
 * @param maxLen size of the output buffer
 * @return 0 on success, -1 on failure
 */
int getValueFromVersionFile(const char *key, char separator, char *valueOut, size_t maxLen)
{
    FILE *fp;
    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    size_t keyLen = strlen(key);
    int found = 0;
    int retValue = -1;

    if (!key || !valueOut || maxLen <= 0) {
        mfrlib_log("getValueFromVersionFile invalid input.\n");
        return retValue;
    }
    /* check if separator is a printable character */
    if (!isprint(separator)) {
        mfrlib_log("getValueFromVersionFile invalid separator.\n");
        return retValue;
    }

    if (access("/version.txt", F_OK) == -1) {
        mfrlib_log("getValueFromVersionFile /version.txt file not found.\n");
        return retValue;
    }

    fp = fopen("/version.txt", "r");
    if (NULL == fp) {
        mfrlib_log("getValueFromVersionFile fopen failed for /version.txt\n");
        return retValue;
    }

    while ((read = getline(&line, &len, fp)) != -1) {
        if (strncmp(line, key, keyLen) == 0 && line[keyLen] == separator) {
            char *value = line + keyLen + 1;
            while (*value == ' ' && *value != '\0') value++;
            int i = 0;
            while (value[i] != '\0' && value[i] != '\n' && value[i] != '\r' && i < maxLen - 1) {
                valueOut[i] = value[i];
                i++;
            }
            valueOut[i] = '\0';
            found = 1;
            retValue = 0;
            break;
        }
    }
    fclose(fp);

    if (line) {
        free(line);
    }

    if (!found) {
        mfrlib_log("getline failed or key not found in /version.txt\n");
    }

    return retValue;
}

/**
 * @brief Get the MAC address of the bluetooth interface
 * @param bdAddress output buffer to store the MAC address in string format
 * @param maxLen size of the output buffer
 * @return 0 on success, -1 on failure
 */
int getBDAddress(char *bdAddress, size_t maxLen)
{
    FILE *fp = NULL;
    char buffer[MAX_BUF_LEN] = {0};
    char *addr_start = NULL;
    int retVal = -1;

    if (!bdAddress || maxLen < 18) { // Bluetooth address is 17 characters + null terminator
        mfrlib_log("getBDAddress invalid input.\n");
        return retVal;
    }

    fp = popen("hciconfig -a | grep 'BD Address'", "r");
    if (NULL == fp) {
        mfrlib_log("getBDAddress popen failed\n");
        return retVal;
    }

    if (fgets(buffer, sizeof(buffer), fp) != NULL) {
        addr_start = strstr(buffer, "BD Address: ");
        if (addr_start) {
            addr_start += strlen("BD Address: ");
            size_t addr_len = strcspn(addr_start, " \n");
            if (addr_len >= maxLen) {
                addr_len = maxLen - 1;
            }
            strncpy(bdAddress, addr_start, addr_len);
            bdAddress[addr_len] = '\0';
            retVal = 0;
        } else {
            mfrlib_log("getBDAddress BD Address not found in '%s' output.\n", "hciconfig -a | grep 'BD Address'");
        }
    } else {
        mfrlib_log("getBDAddress fgets failed\n");
    }

    pclose(fp);
    return retVal;
}

/**
 * @brief Get the MAC address of the given interface
 * @param iface network interface name
 * @param outMACString output buffer to store the MAC address in string format
 * @param size size of the output buffer
 * @return 0 on success, -1 on failure
 */
int getInterfaceMACString(char *iface, char *outMACString, size_t size)
{
    int fd = -1;
    struct ifreq ifr;
    unsigned char *mac = NULL;
    int retVal = -1;

    // MAC address is 6 bytes long, represented as 2 hex characters per byte and 5 colons.
    if (!iface || !outMACString || size < ((3 * 6) + 1)) {
        mfrlib_log("getInterfaceMACString invalid input.\n");
        return retVal;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == -1) {
        mfrlib_log("getInterfaceMACString socket() call error.\n");
        return retVal;
    }

    strncpy(ifr.ifr_name, iface, IFNAMSIZ-1);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) == -1) {
        mfrlib_log("getInterfaceMACString ioctl() call error.\n");
    } else {
        mac = (unsigned char *)ifr.ifr_hwaddr.sa_data;
        snprintf(outMACString, size, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        retVal = 0;
    }
    close(fd);

    return retVal;
}

/**
 * @brief Get the manufacturer OUI in hex string format
 * @param interface network interface name; for RPI, OUI is 6 bytes of the eth0 MAC address
 * @param ouiHexString output buffer to store the manufacturer OUI in hex string format; should be aleast 7 bytes long
 * @return 0 on success, -1 on failure
 */
int getManufacturerOUIHexString(char *ouiHexString, size_t size)
{
    int retVal = -1;
    char macAddress[MAC_ADDRESS_SIZE] = {0};
    if (!ouiHexString || size < 7) {
        mfrlib_log("getManufacturerOUIHexString invalid input.\n");
        return retVal;
    }
    if (getInterfaceMACString("eth0", macAddress, MAC_ADDRESS_SIZE) == 0) {
        /* take the first 3 and remove the colon from string. eg, e4:5f:01:56:f4:82 -> e45f01 */
        char *ptr = macAddress;
        int i = 0;
        while (*ptr != '\0' && i < 6) {
            if (*ptr != ':') {
                ouiHexString[i++] = *ptr;
            }
            ptr++;
        }
        ouiHexString[i] = '\0';
        retVal = 0;
    }
    return retVal;
}

/**
 * @brief Get the value matching the given key from the device properties file
 * @param keyIn key to search for in the device properties file
 * @param valueOut output buffer to store the value matching the key; should be atleast 50 bytes long
 * @param size size of the output buffer
 * @return 0 on success, -1 on failure
*/
int getValueMatchingKeyFromDevicePropertiesFile(const char *keyIn, char *valueOut, size_t size)
{
    FILE *fp = NULL;
    char buffer[MAX_BUF_LEN] = {0};
    size_t len = 0;
    ssize_t read = 0;
    int ret = -1;
    char *line = NULL;

    if (!keyIn || !valueOut || size <= 0) {
        mfrlib_log("getValueMatchingKeyFromDevicePropertiesFile invalid input.\n");
        return ret;
    }

    if (access("/etc/device.properties", F_OK) != -1) {
        fp = fopen("/etc/device.properties", "r");
        if (NULL == fp) {
            mfrlib_log("getValueMatchingKeyFromDevicePropertiesFile fopen() error.\n");
            return ret;
        }

        while ((read = getline(&line, &len, fp)) != -1) {
            if (strstr(line, keyIn) != NULL) {
                strncpy(buffer, line, MAX_BUF_LEN - 1);
                buffer[MAX_BUF_LEN - 1] = '\0';
                break;
            }
        }

        if (line) {
            free(line);
        }

        if (buffer[0] != '\0') {
            char *value = strchr(buffer, '=');
            if (value) {
                value++;
                int i = 0;
                while (value[i] != '\0' && value[i] != '\n' && value[i] != '\r' && i < size - 1) {
                    valueOut[i] = value[i];
                    i++;
                }
                valueOut[i] = '\0';
                ret = 0;
            }
        }
        fclose(fp);
        mfrlib_log("getValueMatchingKeyFromDevicePropertiesFile key='%s', value='%s'\n", keyIn, valueOut);
    } else {
        mfrlib_log("getValueMatchingKeyFromDevicePropertiesFile device.properties file not found.\n");
    }
    return ret;
}

/**
 * @brief Get the value matching the given key from the CPUINFO file
 * @param keyIn key to search for in the CPUINFO file
 * @param valueOut output buffer to store the value matching the key; should be atleast 50 bytes long
 * @param size size of the output buffer
 * @return 0 on success, -1 on failure
*/
int getValueMatchingKeyFromCPUINFO(const char *keyIn, char *valueOut, size_t size)
{
    FILE *fp = NULL;
    char buffer[MAX_BUF_LEN] = {0};
    size_t len = 0;
    ssize_t read = 0;
    int ret = -1;
    char *line = NULL;

    if (!keyIn || !valueOut || size <= 0) {
        mfrlib_log("getValueMatchingKeyFromCPUINFO invalid input.\n");
        return ret;
    }

    if (access("/proc/cpuinfo", F_OK) != -1) {
        fp = fopen("/proc/cpuinfo", "r");
        if (NULL == fp) {
            mfrlib_log("getValueMatchingKeyFromCPUINFO fopen() error.\n");
            return ret;
        }

        while ((read = getline(&line, &len, fp)) != -1) {
            if (strstr(line, keyIn) != NULL) {
                strncpy(buffer, line, MAX_BUF_LEN - 1);
                buffer[MAX_BUF_LEN - 1] = '\0';
                break;
            }
        }

        if (line) {
            free(line);
        }

        if (buffer[0] != '\0') {
            char *value = strchr(buffer, ':');
            if (value) {
                value++;
                while ((*value == ' ') && (*value != '\0')) {
                    value++;
                }
                int i = 0;
                while (value[i] != '\0' && value[i] != '\n' && value[i] != '\r' && i < size - 1) {
                    valueOut[i] = value[i];
                    i++;
                }
                valueOut[i] = '\0';
                ret = 0;
            }
        }
        fclose(fp);
        mfrlib_log("getValueMatchingKeyFromCPUINFO key='%s', value='%s'\n", keyIn, valueOut);
    } else {
        mfrlib_log("getValueMatchingKeyFromCPUINFO cpuinfo file not found.\n");
    }
    return ret;
}

/*************************************************************************************/
/* MFR API implementation */

/**
 * @brief Free the buffer allocated by mfrGetSerializedData; will be triggered by the calling funtion
 * @param buf buffer to free
 */
void mfrFreeBuffer(char *buf)
{
    if (buf) {
        free(buf);
    }
}

/**
 * @brief Check if the given mfrSerializedType_t is valid
 * @param param mfrSerializedType_t
 * @return true if valid, false otherwise
 * @note Refer https://github.com/rdkcentral/iarmmgrs/blob/main/mfr/include/mfrTypes.h
 */
bool isValidMfrSerializedType(mfrSerializedType_t param) {
    // Check if param is within the valid range of mfrSerializedType_t
    if ((param >= mfrSERIALIZED_TYPE_MANUFACTURER && param < mfrSERIALIZED_TYPE_MAX)
#ifdef PANEL_SERIALIZATION_TYPES
        || (param >= mfrSERIALIZED_TYPE_COREBOARD_SERIALNUMBER && param <= mfrSERIALIZED_TYPE_PANEL_MAX) ||
#endif /* PANEL_SERIALIZATION_TYPES */
    ) {
        return true;
    }
    return false;
}

/**
 * @brief Retrieves serialized Read-Only data from device
 *
 *
 * @param [in] type :  specifies the serialized data type to be read. @see mfrSerializedType_t
 * @param [in] data :  serialized data for the specific type requested. (buffer location, length, and func to free the buffer). @see mfrSerializedData_t
 *
 * @return mfrError_t                      - Status
 * @retval mfrERR_NONE                     - Success
 * @retval mfrERR_NOT_INITIALIZED          - Module is not initialised
 * @retval mfrERR_INVALID_PARAM            - Parameter passed to this function is invalid
 * @retval mfrERR_MEMORY_EXHAUSTED         - memory allocation failure
 * @retval mfrERR_FAILED_CRC_CHECK         - CRC check failed
 * @retval mfrERR_FLASH_READ_FAILED        - Flash read failed
 *
 * @note The serialized data is returned as a byte stream. It is upto the  application to deserialize and make sense of the data returned.
 *  Even if the serialized data returned is "string", the buffer is not required to contain the null-terminator
 *
 * @pre  mfr_init() should be called before calling this API. If this precondition is not met, the API will return mfrERR_NOT_INITIALIZED.
 * @warning  This API is Not thread safe
 *
 */
mfrError_t mfrGetSerializedData(mfrSerializedType_t param, mfrSerializedData_t *data)
{
    char cmd[MAX_BUF_LEN] = {0};
    char buffer[MAX_BUF_LEN] = {0};
    FILE *fp = NULL;
    mfrError_t ret = mfrERR_NONE;

    if (!isLibraryInitialized()) {
        mfrlib_log("mfrGetSerializedData not initialized\n");
        return mfrERR_NOT_INITIALIZED;
    }

    if (!data || !isValidMfrSerializedType(param)) {
        mfrlib_log("Invalid mfrSerializedType_t or data ptr is NULL\n");
        return mfrERR_INVALID_PARAM;
    }

    data->bufLen = 0;

    switch (param) {
    case mfrSERIALIZED_TYPE_MANUFACTURER:
        data->buf = (char *)calloc(MAX_BUF_LEN, sizeof(char));
        if (!data->buf) {
            mfrlib_log("Memory alloc error\n");
            ret = mfrERR_MEMORY_EXHAUSTED;
        } else {
            /* retrieving tag MANUFACTURE from /etc/device.properties */
            if (getValueMatchingKeyFromDevicePropertiesFile("MANUFACTURE", data->buf, MAX_BUF_LEN) == 0) {
                data->bufLen = strlen(data->buf);
                data->freeBuf = mfrFreeBuffer;
                mfrlib_log("Manufacturer= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                mfrlib_log("getValueMatchingKeyFromDevicePropertiesFile failed, return mfrERR_FLASH_READ_FAILED.\n");
                ret = mfrERR_FLASH_READ_FAILED;
            }
        }
        break;
    /* unique identifier of the Manufacturer :: we are using the first 6 chars of the mac address */
    case mfrSERIALIZED_TYPE_MANUFACTUREROUI:
        data->buf = (char *)calloc(MAX_BUF_LEN, sizeof(char));
        if (!data->buf) {
            mfrlib_log("Memory alloc error\n");
            ret = mfrERR_MEMORY_EXHAUSTED;
        } else {
            if (getManufacturerOUIHexString(data->buf, MAX_BUF_LEN) == 0) {
                data->bufLen = strlen(data->buf);
                data->freeBuf = mfrFreeBuffer;
                mfrlib_log("Manufacturer OUI= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                mfrlib_log("getManufacturerOUIHexString failed, return mfrERR_FLASH_READ_FAILED.\n");
                ret = mfrERR_FLASH_READ_FAILED;
            }
        }
        break;
    case mfrSERIALIZED_TYPE_MODELNAME:
        data->buf = (char *)calloc(MAX_BUF_LEN, sizeof(char));
        if (!data->buf) {
            mfrlib_log("Memory alloc error\n");
            ret = mfrERR_MEMORY_EXHAUSTED;
        } else {
            /* retrieving tag DEVICE_NAME from /etc/device.properties */
            if (getValueMatchingKeyFromDevicePropertiesFile("DEVICE_NAME", data->buf, MAX_BUF_LEN) == 0) {
                data->bufLen = strlen(data->buf);
                data->freeBuf = mfrFreeBuffer;
                mfrlib_log("Model Name= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                mfrlib_log("getValueMatchingKeyFromDevicePropertiesFile failed, return mfrERR_FLASH_READ_FAILED.\n");
                ret = mfrERR_FLASH_READ_FAILED;
            }
        }
        break;
    case mfrSERIALIZED_TYPE_DESCRIPTION:
        data->buf = (char *)calloc(MAX_BUF_LEN, sizeof(char));
        if (!data->buf) {
            mfrlib_log("Memory alloc error\n");
            ret = mfrERR_MEMORY_EXHAUSTED;
        } else {
            /* Add description as 'RDKV Reference Device' */
            strncpy(data->buf, defaultDescription, ((sizeof(defaultDescription) < MAX_BUF_LEN) ? sizeof(defaultDescription) : MAX_BUF_LEN));
            data->bufLen = strlen(data->buf);
            data->freeBuf = mfrFreeBuffer;
            mfrlib_log("Description= '%s', len=%d\n", data->buf, data->bufLen);
        }
        break;
    case mfrSERIALIZED_TYPE_PRODUCTCLASS:
        data->buf = (char *)calloc(MAX_BUF_LEN, sizeof(char));
        if (!data->buf) {
            mfrlib_log("Memory alloc error\n");
            ret = mfrERR_MEMORY_EXHAUSTED;
        } else {
            /* Add product class as 'RDKV' */
            strncpy(data->buf, defaultProductClass, ((sizeof(defaultProductClass) < MAX_BUF_LEN) ? sizeof(defaultProductClass) : MAX_BUF_LEN));
            data->bufLen = strlen(data->buf);
            data->freeBuf = mfrFreeBuffer;
            mfrlib_log("Product Class= '%s', len=%d\n", data->buf, data->bufLen);
        }
        break;
    case mfrSERIALIZED_TYPE_SERIALNUMBER:
    case mfrSERIALIZED_TYPE_MANUFACTURING_SERIALNUMBER:
        data->buf = (char *)calloc(MAX_BUF_LEN, sizeof(char));
        if (!data->buf) {
            mfrlib_log("Memory alloc error\n");
            ret = mfrERR_MEMORY_EXHAUSTED;
        } else {
            /* retrieving tag SERIAL_NUMBER from /etc/device.properties */
            if (getValueMatchingKeyFromCPUINFO("Serial", data->buf, MAX_BUF_LEN) == 0) {
                data->bufLen = strlen(data->buf);
                data->freeBuf = mfrFreeBuffer;
                mfrlib_log("Serial Number= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                mfrlib_log("getValueMatchingKeyFromCPUINFO failed, return mfrERR_FLASH_READ_FAILED.\n");
                ret = mfrERR_FLASH_READ_FAILED;
            }
        }
        break;
    case mfrSERIALIZED_TYPE_HARDWAREVERSION:
        data->buf = (char *)calloc(MAX_BUF_LEN, sizeof(char));
        if (!data->buf) {
            mfrlib_log("Memory alloc error\n");
            ret = mfrERR_MEMORY_EXHAUSTED;
        } else {
            /* retrieving tag REVISION from /etc/device.properties */
            if (getValueMatchingKeyFromCPUINFO("Revision", data->buf, MAX_BUF_LEN) == 0) {
                data->bufLen = strlen(data->buf);
                data->freeBuf = mfrFreeBuffer;
                mfrlib_log("Hardware Version= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                mfrlib_log("getValueMatchingKeyFromCPUINFO failed, return mfrERR_FLASH_READ_FAILED.\n");
                ret = mfrERR_FLASH_READ_FAILED;
            }
        }
        break;
    case mfrSERIALIZED_TYPE_DEVICEMAC:
    case mfrSERIALIZED_TYPE_ETHERNETMAC:
    case mfrSERIALIZED_TYPE_ESTBMAC:
        data->buf = (char *)calloc(MAX_BUF_LEN, sizeof(char));
        if (!data->buf) {
            mfrlib_log("Memory alloc error\n");
            ret = mfrERR_MEMORY_EXHAUSTED;
        } else {
            if (getInterfaceMACString("eth0", data->buf, MAX_BUF_LEN) == 0) {
                data->bufLen = strlen(data->buf);
                data->freeBuf = mfrFreeBuffer;
                mfrlib_log("Device MAC= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                mfrlib_log("getInterfaceMACString failed, return mfrERR_FLASH_READ_FAILED.\n");
                ret = mfrERR_FLASH_READ_FAILED;
            }
        }
        break;
    case mfrSERIALIZED_TYPE_WIFIMAC:
        data->buf = (char *)calloc(MAX_BUF_LEN, sizeof(char));
        if (!data->buf) {
            mfrlib_log("Memory alloc error\n");
            ret = mfrERR_MEMORY_EXHAUSTED;
        } else {
            if (getInterfaceMACString("wlan0", data->buf, MAX_BUF_LEN) == 0) {
                data->bufLen = strlen(data->buf);
                data->freeBuf = mfrFreeBuffer;
                mfrlib_log("WiFi MAC= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                mfrlib_log("getInterfaceMACString failed, return mfrERR_FLASH_READ_FAILED.\n");
                ret = mfrERR_FLASH_READ_FAILED;
            }
        }
        break;
    case mfrSERIALIZED_TYPE_SOFTWAREVERSION:
        /* return defaultSoftwareVersion */
        data->buf = (char *)calloc(MAX_BUF_LEN, sizeof(char));
        if (!data->buf) {
            mfrlib_log("Memory alloc error\n");
            ret = mfrERR_MEMORY_EXHAUSTED;
        } else {
            strncpy(data->buf, defaultSoftwareVersion, ((sizeof(defaultSoftwareVersion) < MAX_BUF_LEN) ? sizeof(defaultSoftwareVersion) : MAX_BUF_LEN));
            data->bufLen = strlen(data->buf);
            data->freeBuf = mfrFreeBuffer;
            mfrlib_log("Software Version= '%s', len=%d\n", data->buf, data->bufLen);
        }
        break;
    case mfrSERIALIZED_TYPE_MOCAMAC:
        data->buf = (char *)calloc(MAX_BUF_LEN, sizeof(char));
        if (!data->buf) {
            mfrlib_log("Memory alloc error\n");
            ret = mfrERR_MEMORY_EXHAUSTED;
        } else {
            /* get MOCA_INTERFACE from device.properties and retieve its MAC */
            char mocaInterface[16] = {0};
            if (getValueMatchingKeyFromDevicePropertiesFile("MOCA_INTERFACE", mocaInterface, sizeof(mocaInterface)) == 0) {
                if (getInterfaceMACString(mocaInterface, data->buf, MAX_BUF_LEN) == 0) {
                    data->bufLen = strlen(data->buf);
                    data->freeBuf = mfrFreeBuffer;
                    mfrlib_log("MOCA MAC= '%s', len=%d\n", data->buf, data->bufLen);
                } else {
                    mfrFreeBuffer(data->buf);
                    mfrlib_log("getInterfaceMACString failed, return mfrERR_FLASH_READ_FAILED.\n");
                    ret = mfrERR_FLASH_READ_FAILED;
                }
            } else {
                mfrFreeBuffer(data->buf);
                mfrlib_log("getValueMatchingKeyFromDevicePropertiesFile failed, return mfrERR_FLASH_READ_FAILED.\n");
                ret = mfrERR_FLASH_READ_FAILED;
            }
        }
        break;
    case mfrSERIALIZED_TYPE_BLUETOOTHMAC:
        data->buf = (char *)calloc(MAX_BUF_LEN, sizeof(char));
        if (!data->buf) {
            mfrlib_log("Memory alloc error\n");
            ret = mfrERR_MEMORY_EXHAUSTED;
        } else {
            if (getBDAddress(data->buf, MAX_BUF_LEN) == 0) {
                data->bufLen = strlen(data->buf);
                data->freeBuf = mfrFreeBuffer;
                mfrlib_log("Bluetooth MAC= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                mfrlib_log("getBDAddress failed, return mfrERR_FLASH_READ_FAILED.\n");
                ret = mfrERR_FLASH_READ_FAILED;
            }
        }
        break;
    case mfrSERIALIZED_TYPE_HWID:
    case mfrSERIALIZED_TYPE_MODELNUMBER:
        /* Read cpuinfo and use Revision */
        data->buf = (char *)calloc(MAX_BUF_LEN, sizeof(char));
        if (!data->buf) {
            mfrlib_log("Memory alloc error\n");
            ret = mfrERR_MEMORY_EXHAUSTED;
        } else {
            if (getValueMatchingKeyFromCPUINFO("Revision", data->buf, MAX_BUF_LEN) == 0) {
                data->bufLen = strlen(data->buf);
                data->freeBuf = mfrFreeBuffer;
                mfrlib_log("HWID= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                mfrlib_log("getValueMatchingKeyFromCPUINFO failed, return mfrERR_FLASH_READ_FAILED.\n");
                ret = mfrERR_FLASH_READ_FAILED;
            }
        }
        break;
    case mfrSERIALIZED_TYPE_SOC_ID:
        /* Read cpuinfo and use Hardware */
        data->buf = (char *)calloc(MAX_BUF_LEN, sizeof(char));
        if (!data->buf) {
            mfrlib_log("Memory alloc error\n");
            ret = mfrERR_MEMORY_EXHAUSTED;
        } else {
            if (getValueMatchingKeyFromCPUINFO("Hardware", data->buf, MAX_BUF_LEN) == 0) {
                data->bufLen = strlen(data->buf);
                data->freeBuf = mfrFreeBuffer;
                mfrlib_log("SOC ID= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                mfrlib_log("getValueMatchingKeyFromCPUINFO failed, return mfrERR_FLASH_READ_FAILED.\n");
                ret = mfrERR_FLASH_READ_FAILED;
            }
        }
        break;
    case mfrSERIALIZED_TYPE_IMAGENAME:
        /* Read /version.txt and extract 'imagename' */
        data->buf = (char *)calloc(MAX_BUF_LEN, sizeof(char));
        if (!data->buf) {
            mfrlib_log("Memory alloc error\n");
            ret = mfrERR_MEMORY_EXHAUSTED;
        } else {
            if (getValueFromVersionFile("imagename", ':', data->buf, MAX_BUF_LEN) == 0) {
                data->bufLen = strlen(data->buf);
                data->freeBuf = mfrFreeBuffer;
                mfrlib_log("Image Name= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                mfrlib_log("getValueFromVersionFile failed, return mfrERR_FLASH_READ_FAILED.\n");
                ret = mfrERR_FLASH_READ_FAILED;
            }
        }
        break;
    case mfrSERIALIZED_TYPE_PROVISIONINGCODE:
    case mfrSERIALIZED_TYPE_FIRSTUSEDATE:
    case mfrSERIALIZED_TYPE_PDRIVERSION:
    case mfrSERIALIZED_TYPE_HDMIHDCP:
    case mfrSERIALIZED_TYPE_MAX:
    case mfrSERIALIZED_TYPE_WPSPIN:
    case mfrSERIALIZED_TYPE_RF4CEMAC:
    case mfrSERIALIZED_TYPE_PROVISIONED_MODELNAME:
    case mfrSERIALIZED_TYPE_PMI:
    case mfrSERIALIZED_TYPE_IMAGETYPE:
    case mfrSERIALIZED_TYPE_BLVERSION:
    case mfrSERIALIZED_TYPE_REGION:
    case mfrSERIALIZED_TYPE_BDRIVERSION:
    case mfrSERIALIZED_TYPE_LED_WHITE_LEVEL:
    case mfrSERIALIZED_TYPE_LED_PATTERN:
    default:
        /* Does not have any data. Report unsupported. */
        mfrlib_log("Unsupported mfrSerializedType_t '%d'\n", param);
        ret = mfrERR_OPERATION_NOT_SUPPORTED;
        break;
    }
    return ret;
}

/**
 * @brief Sets the read write Serialization data on device
 *
 * @param [in] type :  specifies the serialized data type to write. @see mfrSerializedType_t
 * @param [in] data :  serialized data to set for the specific type requested. (buffer location, length, and func to free the buffer). @see mfrSerializedData_t
 *
 * @return mfrError_t                       - Status
 * @retval mfrERR_NONE                      - Success
 * @retval mfrERR_NOT_INITIALIZED           - Module is not initialised
 * @retval mfrERR_INVALID_PARAM             - Parameter passed to this function is invalid
 * @retval mfrERR_MEMORY_EXHAUSTED          - memory allocation failure
 * @retval mfrERR_FAILED_CRC_CHECK          - CRC check failed
 * @retval mfrERR_WRITE_FLASH_FAILED        - Flash write failed
 * @retval mfrERR_FLASH_READ_FAILED        - Flash read failed
 * @retval mfrERR_FLASH_VERIFY_FAILED       - Flash verification failed
 *
 * @pre  mfr_init() should be called before calling this API. If this precondition is not met, the API will return mfrERR_NOT_INITIALIZED.
 * @warning  This API is Not thread safe
 *
 */
mfrError_t mfrSetSerializedData( mfrSerializedType_t type,  mfrSerializedData_t *data)
{
    if (!isLibraryInitialized()) {
        mfrlib_log("isLibraryInitialized not initialized\n");
        return mfrERR_NOT_INITIALIZED;
    }

    if (!data || !isValidMfrSerializedType(type)) {
        mfrlib_log("Invalid mfrSerializedType_t or data ptr is NULL\n");
        return mfrERR_INVALID_PARAM;
    }

    return mfrERR_OPERATION_NOT_SUPPORTED;
}

/**
 * @brief Deletes the PDRI image if it is present
 *
 * @return mfrError_t                      - Status
 * @retval mfrERR_NONE                     - Success
 * @retval mfrERR_NOT_INITIALIZED          - Module is not initialised
 * @retval mfrERR_WRITE_FLASH_FAILED       - Flash write failed
 * @retval mfrERR_FLASH_VERIFY_FAILED      - Flash verification failed
 *
 * @pre  mfr_init() should be called before calling this API. If this precondition is not met, the API will return mfrERR_NOT_INITIALIZED.
 * @warning  This API is Not thread safe
 *
 */
mfrError_t mfrDeletePDRI()
{
    if (!isLibraryInitialized()) {
        mfrlib_log("isLibraryInitialized not initialized\n");
        return mfrERR_NOT_INITIALIZED;
    }
    return mfrERR_OPERATION_NOT_SUPPORTED;
}

/**
 * @brief Deletes the platform images. Deletes the main image from primary and secondary bank
 *
 * @return mfrError_t                      - Status
 * @retval mfrERR_NONE                     - Success
 * @retval mfrERR_NOT_INITIALIZED          - Module is not initialised
 * @retval mfrERR_WRITE_FLASH_FAILED       - Flash write failed
 * @retval mfrERR_FLASH_VERIFY_FAILED      - Flash verification failed
 *
 * @pre  mfr_init() should be called before calling this API. If this precondition is not met, the API will return mfrERR_NOT_INITIALIZED.
 * @warning  This API is Not thread safe
 *
 */
mfrError_t mfrScrubAllBanks()
{
    if (!isLibraryInitialized()) {
        mfrlib_log("isLibraryInitialized not initialized\n");
        return mfrERR_NOT_INITIALIZED;
    }
    return mfrERR_OPERATION_NOT_SUPPORTED;
}

bool isValidMfrBLPattern(mfrBlPattern_t pattern)
{
    if (pattern >= mfrBL_PATTERN_NORMAL && pattern < mfrBL_PATTERN_MAX) {
        return true;
    }
    return false;
}

static int copyFile(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in == -1) return -1;

    struct stat st;
    if (fstat(in, &st) == -1) { close(in); return -1; }

    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
    if (out == -1) { close(in); return -1; }

    off_t remaining = st.st_size;
    loff_t off_in = 0, off_out = 0;
    int ret = 0;
    while (remaining > 0) {
        ssize_t copied = copy_file_range(in, &off_in, out, &off_out, (size_t)remaining, 0);
        if (copied == -1) { ret = -1; break; }
        remaining -= copied;
    }

    close(in);
    close(out);
    return ret;
}

/*
 * Valid values for act_led_dtparam:
 *
 *  Parameter                            Description
 *  -----------------------------------  ----------------------------
 *  dtparam=act_led_trigger=none         Disable the LED (stays off)
 *  dtparam=act_led_trigger=default-on   Always on
 *  dtparam=act_led_trigger=heartbeat    Heartbeat blink
 *  dtparam=act_led_trigger=mmc0         SD card activity (default)
 *  dtparam=act_led_activelow=on         Invert logic (active-low)
 */
bool isValidActLEDParam(const char *param)
{
	const char *validParams[] = {
		"dtparam=act_led_trigger=none",
		"dtparam=act_led_trigger=default-on",
		"dtparam=act_led_trigger=heartbeat",
		"dtparam=act_led_trigger=mmc0"
	};
	size_t numValidParams = sizeof(validParams) / sizeof(validParams[0]);
	for (size_t i = 0; i < numValidParams; i++) {
		if (strcmp(param, validParams[i]) == 0) {
			return true;
		}
	}
	return false;
}

mfrError_t updateBootConfigFile(const char *act_led_dtparam)
{
    FILE *fp = NULL;
    int found = 0;
    int retVal = mfrERR_NONE;
    char *fileContents = NULL;
    char *line = NULL;
    size_t lineLen = 0;
    ssize_t nread;
    size_t newSize = 0;
    int written = 0;
    const char *key = "dtparam=act_led_trigger=";

    if (!act_led_dtparam || !isValidActLEDParam(act_led_dtparam)) {
        mfrlib_log("updateBootConfigFile invalid input or unsupported parameter\n");
        return mfrERR_INVALID_PARAM;
    }

    // Open and exclusively lock the file first so that backup, modification,
    // and any restore all happen atomically with respect to other flock() callers.
    // flock() is advisory; all writers must cooperate by also calling flock().
    fp = fopen(BOOT_CONFIG_FILE, "r+");
    if (NULL == fp) {
        mfrlib_log("updateBootConfigFile fopen() error for %s\n", BOOT_CONFIG_FILE);
        return mfrERR_WRITE_FLASH_FAILED;
    }
    if (flock(fileno(fp), LOCK_EX) != 0) {
        mfrlib_log("updateBootConfigFile flock() error for %s\n", BOOT_CONFIG_FILE);
        fclose(fp);
        return mfrERR_WRITE_FLASH_FAILED;
    }

    // Back up the original file to /opt while holding the lock.
    if (copyFile(BOOT_CONFIG_FILE, BOOT_CONFIG_BACKUP_FILE) != 0) {
        mfrlib_log("updateBootConfigFile failed to create backup at %s\n", BOOT_CONFIG_BACKUP_FILE);
        fclose(fp);
        return mfrERR_WRITE_FLASH_FAILED;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        remove(BOOT_CONFIG_BACKUP_FILE);
        return mfrERR_WRITE_FLASH_FAILED;
    }
    long fsize = ftell(fp);
    if (fsize < 0) {
        fclose(fp);
        remove(BOOT_CONFIG_BACKUP_FILE);
        return mfrERR_WRITE_FLASH_FAILED;
    }
    rewind(fp);

    // Allocate buffer large enough for original content plus a possible new line.
    fileContents = (char *)malloc((size_t)fsize + MAX_BUF_LEN + 2);
    if (!fileContents) {
        fclose(fp);
        remove(BOOT_CONFIG_BACKUP_FILE);
        return mfrERR_WRITE_FLASH_FAILED;
    }

    // Read line by line; replace dtparam=act_led_trigger= if found, else append later.
    while ((nread = getline(&line, &lineLen, fp)) != -1) {
        if (!found && strncmp(line, key, strlen(key)) == 0) {
            written = snprintf(fileContents + newSize, MAX_BUF_LEN, "%s\n", act_led_dtparam);
            if (written < 0 || written >= MAX_BUF_LEN) {
                retVal = mfrERR_WRITE_FLASH_FAILED;
                goto cleanup;
            }
            newSize += (size_t)written;
            found = 1;
        } else {
            memcpy(fileContents + newSize, line, (size_t)nread);
            newSize += (size_t)nread;
        }
    }

    if (!found) {
        // Append the dtparam line at the end of the file.
        written = snprintf(fileContents + newSize, MAX_BUF_LEN, "%s\n", act_led_dtparam);
        if (written < 0 || written >= MAX_BUF_LEN) {
            retVal = mfrERR_WRITE_FLASH_FAILED;
            goto cleanup;
        }
        newSize += (size_t)written;
    }

    rewind(fp);
    if (fwrite(fileContents, 1, newSize, fp) != newSize) {
        mfrlib_log("updateBootConfigFile fwrite() error; restoring from backup\n");
        retVal = mfrERR_WRITE_FLASH_FAILED;
    } else if (ftruncate(fileno(fp), (off_t)newSize) != 0) {
        mfrlib_log("updateBootConfigFile ftruncate() error; restoring from backup\n");
        retVal = mfrERR_WRITE_FLASH_FAILED;
    }

    // On any write failure, restore from the backup file while still holding the lock.
    if (retVal != mfrERR_NONE) {
        if (copyFile(BOOT_CONFIG_BACKUP_FILE, BOOT_CONFIG_FILE) != 0) {
            mfrlib_log("updateBootConfigFile restore failed; %s may be corrupted\n", BOOT_CONFIG_FILE);
        }
    }

cleanup:
    free(line);
    free(fileContents);
    if (fp) {
        fclose(fp); // releases flock
    }
    remove(BOOT_CONFIG_BACKUP_FILE);
    sync();
    return retVal;
}

/**
 * @brief Sets bootloader LED pattern
 *
 * This function stores the bootup pattern in the persistance storage for bootloader to read
 * and control the front panel LED and/or TV backlight sequence on bootup
 *
 * @param [in] pattern : options are defined by enum mfrBlPattern_t. @see mfrBlPattern_t
 *
 * @return mfrError_t                      - Status
 * @retval mfrERR_NONE                     - Success
 * @retval mfrERR_NOT_INITIALIZED          - Module is not initialised
 * @retval mfrERR_INVALID_PARAM            - Parameter passed to this function is invalid
 * @retval mfrERR_WRITE_FLASH_FAILED       - Flash write failed
 * @retval mfrERR_FLASH_VERIFY_FAILED      - Flash verification failed
 *
 * @pre  mfr_init() should be called before calling this API. If this precondition is not met, the API will return mfrERR_NOT_INITIALIZED.
 * @warning  This API is Not thread safe
 *
 */
mfrError_t mfrSetBootloaderPattern(mfrBlPattern_t pattern)
{
    mfrError_t returnStatus = mfrERR_NONE;
    if (!isLibraryInitialized()) {
        mfrlib_log("isLibraryInitialized not initialized\n");
        return mfrERR_NOT_INITIALIZED;
    }

    if (!isValidMfrBLPattern(pattern)) {
        mfrlib_log("mfrSetBootloaderPattern Invalid mfrBlPattern_t\n");
        return mfrERR_INVALID_PARAM;
    }

    switch (pattern) {
        case mfrBL_PATTERN_NORMAL:
            // Normal boot loader pattern - enable both LOGO as well LED ON during boot up.
            returnStatus = updateBootConfigFile("dtparam=act_led_trigger=default-on");
            break;
        case mfrBL_PATTERN_SILENT:
            // Silent boot loader pattern - keep the LED off.
            returnStatus = updateBootConfigFile("dtparam=act_led_trigger=none");
            break;
        case mfrBL_PATTERN_SILENT_LED_ON:
            // silent LED on pattern - enable only LED and disable LOGO during this boot up
            returnStatus = updateBootConfigFile("dtparam=act_led_trigger=default-on");
            break;
        case mfrBL_PATTERN_LOGO_DISABLED:
            // Logo disabled pattern - keep the LOGO off
            mfrlib_log("mfrSetBootloaderPattern Logo disabled pattern\n");
            break;
        default:
            mfrlib_log("mfrSetBootloaderPattern Unsupported mfrBlPattern_t\n");
            returnStatus = mfrERR_OPERATION_NOT_SUPPORTED;
    }

    return returnStatus;
}

/**
 * @brief API to update Primary Splash screen Image and to override the default the Splash screen image
 *
 * @param [in] path : char pointer which holds the path of input bootloader OSD image.
 *
 * @return mfrError_t                      - Status
 * @retval mfrERR_NONE                     - Success
 * @retval mfrERR_NOT_INITIALIZED          - Module is not initialised
 * @retval mfrERR_INVALID_PARAM            - Parameter passed to this function is invalid
 * @retval mfrERR_IMAGE_FILE_OPEN_FAILED   - Failed to open the downloaded splash screen file
 * @retval mfrERR_MEMORY_EXHAUSTED         - memory allocation failure
 *
 * @pre  mfr_init() should be called before calling this API. If this precondition is not met, the API will return mfrERR_NOT_INITIALIZED.
 * @warning  This API is Not thread safe
 *
 */
mfrError_t mfrSetBlSplashScreen(const char *path)
{
    if (!isLibraryInitialized()) {
        mfrlib_log("isLibraryInitialized not initialized\n");
        return mfrERR_NOT_INITIALIZED;
    }

    if (!path) {
        mfrlib_log("mfrSetBlSplashScreen invalid input\n");
        return mfrERR_INVALID_PARAM;
    }
    return mfrERR_OPERATION_NOT_SUPPORTED;
}

/**
 * @brief API to clear the primary Splash screen Image and to make
 * use of default Splash screen image
 *
 * @return mfrError_t                      - Status
 * @retval mfrERR_NONE                     - Success
 * @retval mfrERR_NOT_INITIALIZED          - Module is not initialised
 * @retval mfrERR_IMAGE_FILE_OPEN_FAILED   - Failed to open the downloaded splash screen file
 * @retval mfrERR_MEMORY_EXHAUSTED         - memory allocation failure
 *
 * @pre  mfr_init() should be called before calling this API. If this precondition is not met, the API will return mfrERR_NOT_INITIALIZED.
 * @warning  This API is Not thread safe
 *
 */
mfrError_t mfrClearBlSplashScreen(void)
{
    if (!isLibraryInitialized()) {
        mfrlib_log("isLibraryInitialized not initialized\n");
        return mfrERR_NOT_INITIALIZED;
    }

    return mfrERR_OPERATION_NOT_SUPPORTED;
}

/**
* @brief API to retrive the secure time from TEE
*
* @param [in] params : unit32 timeptr to get the UTC time in seconds
*
* @return Error Code:  Return mfrERR_NONE if operation is successful, mfrERR_GENERAL if it fails
*/
mfrError_t mfrGetSecureTime(uint32_t *timeptr)
{
    if (!isLibraryInitialized()) {
        mfrlib_log("isLibraryInitialized not initialized\n");
        return mfrERR_NOT_INITIALIZED;
    }

    if (!timeptr) {
        mfrlib_log("mfrGetSecureTime invalid input\n");
        return mfrERR_INVALID_PARAM;
    }
#if USE_HEADER_SPECIFIC_RETURN_STATUS
    return mfrERR_GENERAL;
#else /* !USE_HEADER_SPECIFIC_RETURN_STATUS */
    return mfrERR_OPERATION_NOT_SUPPORTED;
#endif /* !USE_HEADER_SPECIFIC_RETURN_STATUS */
}

/**
* @brief API to set the secure time from TEE
*
* @param [in] params : unit32 timeptr to set the UTC time in seconds
*
* @return Error Code:  Return mfrERR_NONE if operation is successful, mfrERR_GENERAL if it fails
*/
mfrError_t mfrSetSecureTime(uint32_t *timeptr)
{
    if (!isLibraryInitialized()) {
        mfrlib_log("isLibraryInitialized not initialized\n");
        return mfrERR_NOT_INITIALIZED;
    }

    if (!timeptr) {
        mfrlib_log("mfrSetSecureTime invalid input\n");
        return mfrERR_INVALID_PARAM;
    }
#if USE_HEADER_SPECIFIC_RETURN_STATUS
    return mfrERR_GENERAL;
#else /* !USE_HEADER_SPECIFIC_RETURN_STATUS */
    return mfrERR_OPERATION_NOT_SUPPORTED;
#endif /* !USE_HEADER_SPECIFIC_RETURN_STATUS */
}

/**
 * @brief API to set the fsr flag into the emmc raw area
 *
 * @param [in] params : uint16_t fsrflag to set the FSR flag
 *
 * @return mfrError_t                      - Status
 * @retval mfrERR_NONE                     - Success
 * @retval mfrERR_NOT_INITIALIZED          - Module is not initialised
 * @retval mfrERR_INVALID_PARAM            - Parameter passed to this function is invalid
 * @return Error Code:  Return mfrERR_NONE if operation is successful, mfrERR_GENERAL if it fails
 *
 **/
mfrError_t mfrSetFSRflag(uint16_t *newFsrFlag)
{
    if (!isLibraryInitialized()) {
        mfrlib_log("isLibraryInitialized not initialized\n");
        return mfrERR_NOT_INITIALIZED;
    }

    if (!newFsrFlag) {
        mfrlib_log("mfrSetFSRflag invalid input\n");
        return mfrERR_INVALID_PARAM;
    }
#if USE_HEADER_SPECIFIC_RETURN_STATUS
    return mfrERR_GENERAL;
#else /* !USE_HEADER_SPECIFIC_RETURN_STATUS */
    return mfrERR_OPERATION_NOT_SUPPORTED;
#endif /* !USE_HEADER_SPECIFIC_RETURN_STATUS */
}

/**
* @brief API to get the fsr flag from emmc
*
* @param [in] params : unit32 fsrflag to get the FSR flag
*
* @return Error Code:  Return mfrERR_NONE if operation is successful, mfrERR_GENERAL if it fails
*/
mfrError_t mfrGetFSRflag(uint16_t *newFsrFlag)
{
    if (!isLibraryInitialized()) {
        mfrlib_log("isLibraryInitialized not initialized\n");
        return mfrERR_NOT_INITIALIZED;
    }

    if (!newFsrFlag) {
        mfrlib_log("mfrGetFSRflag invalid input\n");
        return mfrERR_INVALID_PARAM;
    }
#if USE_HEADER_SPECIFIC_RETURN_STATUS
    return mfrERR_GENERAL;
#else /* !USE_HEADER_SPECIFIC_RETURN_STATUS */
    return mfrERR_OPERATION_NOT_SUPPORTED;
#endif /* !USE_HEADER_SPECIFIC_RETURN_STATUS */
}

bool isValidMfrImageType(mfrImageType_t type) {
    if (type >= mfrIMAGE_TYPE_CDL && type < mfrIMAGE_TYPE_MAX) {
        return true;
    }
    return false;
}

/**
 * @brief Initializes the MFR library
 *
 * This function will initialize all the respective internal components responsible for MFR functionalities.
 * This API need to be called before any other APIs in this module
 *
 * @return mfrError_t                      - Status
 * @retval mfrERR_NONE                     - Success
 * @retval mfrERR_ALREADY_INITIALIZED      - Module is already initialised
 * @retval mfrERR_MEMORY_EXHAUSTED         - memory allocation failure
 *
 * @warning  This API is Not thread safe
 *
 */
mfrError_t mfr_init(void)
{
    configMFRLibLogging();

    if (isInitialized) {
        mfrlib_log("mfr_init already initialized\n");
        return mfrERR_ALREADY_INITIALIZED;
    }

#ifdef ENABLE_SINGLE_INSTANCE_LOCK
    if (acquireLock() == -1) {
        mfrlib_log("mfr_init acquireLock failed\n");
        return mfrERR_ALREADY_INITIALIZED;
    }
#endif /* ENABLE_SINGLE_INSTANCE_LOCK */

    isInitialized = 1;
    return mfrERR_NONE;
}

/**
 * @brief Uninitializes the MFR library
 *
 * This function will uninitialize all the respective internal components responsible for MFR functionalities.
 *
 * @return mfrError_t                      - Status
 * @retval mfrERR_NONE                     - Success
 * @retval mfrERR_NOT_INITIALIZED          - Module is not initialised
 *
 * @warning  This API is Not thread safe
 *
 */
mfrError_t mfr_term(void)
{
    if (!isInitialized) {
        mfrlib_log("mfr_term not initialized\n");
        return mfrERR_NOT_INITIALIZED;
    }

#ifdef ENABLE_SINGLE_INSTANCE_LOCK
    if (releaseLock() == -1) {
        mfrlib_log("mfr_term releaseLock failed\n");
        return mfrERR_NOT_INITIALIZED;
    }
#endif /* ENABLE_SINGLE_INSTANCE_LOCK */

    isInitialized = 0;
    return mfrERR_NONE;
}

/**
 * @brief Writes the image into flash
 *
 *    The process should follow these major steps:
 *    1) Verify the validity of the image and flash
 *    2) Update boot params and switch banks to prepare for a reboot event
 *    3) All upgrades should be done in the alternate bank. The current bank should not be disturbed
 *
 *    State Transition:
 *    0) Before the API is invoked, the Upgrade process should be in PROGRESS_NOT_STARTED state
 *    1) After the API returns with success, the Upgrade process moves to PROGRESS_STARTED state
 *    2) After the API returns with error,   the Upgrade process stays in PROGRESS_NOT_STARTED state. Notify function will not be invoked
 *    3) The notify function is called at regular interval with process = PROGRESS_STARTED
 *    4) The last invocation of notify function should have either progress = PROGRESS_COMPLETED or progress = PROGRESS_ABORTED with error code set
 *
 *  @note mfrWriteImage() should work without any issue when device transition to DEEPSLEEP state and Wakeup. During DEEPSLEEP state processor will
 * cache all the pc and stack state and will enter to low power state. On wakeup system will use the saved pc and stack and resume from the same point.
 *
 * @param [in] name :  the filename of the image file
 * @param [in] path :  the path of the image file in the file system
 * @param [in] type :  the type (format, signature type) of the image.  This can dictate the handling of the image within the MFR library. @see mfrImageType_t
 * @param[in] notify: function to provide status of the image flashing process.  @see mfrUpgradeStatusNotify_t
 *
 *
 * @return mfrError_t                              - Status
 *
 * @retval mfrERR_NONE                             - Success
 * @retval mfrERR_NOT_INITIALIZED                  - Module is not initialised
 * @retval mfrERR_INVALID_PARAM                    - Parameter passed to this function is invalid
 * @retval mfrERR_MEMORY_EXHAUSTED                 - memory allocation failure
 * @retval mfrERR_FAILED_CRC_CHECK                 - CRC is failed
 * @retval mfrERR_WRITE_FLASH_FAILED               - Flash write failed
 * @retval mfrERR_FLASH_VERIFY_FAILED              - Flash verification failed
 * @retval mfrERR_BAD_IMAGE_HEADER                 - Image header is corrupted
 * @retval mfrERR_IMPROPER_SIGNATURE               - Image signature is invalid
 * @retval mfrERR_IMAGE_TOO_BIG                    - Image size is more than allocated maximum
 * @retval mfrERR_FAILED_INVALID_SIGNING_TIME      - Image signing time invalid
 * @retval mfrERR_FAILED_IMAGE_SVN_OLDER           - software version number is older than existing image
 * @retval mfrERR_FAILED_SAME_DRI_CODE_VERSION     - DRI code version is same
 * @retval mfrERR_FAILED_SAME_PCI_CODE_VERSION     - PCI code version is same
 * @retval mfrERR_IMAGE_FILE_OPEN_FAILED           - Not able to open the input image file
 * @retval mfrERR_GET_FLASHED_IMAGE_DETAILS_FAILED - Not able to get the current image version details
 *
 *
 * @pre  mfr_init() should be called before calling this API. If this precondition is not met, the API will return mfrERR_NOT_INITIALIZED. .
 * @warning  This API is Not thread safe
 *
 */
mfrError_t mfrWriteImage(const char *name,  const char *path, mfrImageType_t type,  mfrUpgradeStatusNotify_t notify)
{
    if (!isLibraryInitialized()) {
        mfrlib_log("isLibraryInitialized not initialized\n");
        return mfrERR_NOT_INITIALIZED;
    }

    if (!name || !path || !isValidMfrImageType(type)) {
        mfrlib_log("mfrWriteImage invalid input\n");
        return mfrERR_INVALID_PARAM;
    }
    // TODO: change FlashApp.sh logic to use mfrWriteImage
    return mfrERR_OPERATION_NOT_SUPPORTED;
}

/****************************** MFR WIFI APIs ********************************/

/**
 * @brief Retrieves the saved SSID name, password, and security mode from the MFR persistence
 *
 * @param pData [out] : out parameter to get the saved wifi credentials. @see WIFI_DATA
 *
 * @return    WIFI_API_RESULT                            - Status
 * @retval    WIFI_API_RESULT_SUCCESS                    - Success
 * @retval    WIFI_API_RESULT_NOT_INITIALIZED            - Not initialized
 * @retval    WIFI_API_RESULT_OPERATION_NOT_SUPPORTED    - Operation not supported
 * @retval    WIFI_API_RESULT_NULL_PARAM                 - Null param
 * @retval    WIFI_API_RESULT_READ_WRITE_FAILED          - flash operation failed
 *
 * @pre  mfr_init() should be called before calling this API. If this precondition is not met, the API will return WIFI_API_RESULT_NOT_INITIALIZED.
 * @warning  This API is NOT thread safe. Caller shall handle the concurrency
 * @see  WIFI_SetCredentials()
 *
 */
WIFI_API_RESULT WIFI_GetCredentials(WIFI_DATA *pData)
{
    if (!isLibraryInitialized()) {
        mfrlib_log("isLibraryInitialized not initialized\n");
        return WIFI_API_RESULT_NOT_INITIALIZED;
    }

    if (NULL == pData) {
        return WIFI_API_RESULT_NULL_PARAM;
    }

    return WIFI_API_RESULT_OPERATION_NOT_SUPPORTED;
}

/**
 * @brief Sets wifi ssid name, password and the security mode in the MFR persistance storage
 *
 * @param pData [in] : Sets the ssid credentials. @see WIFI_DATA
 *
 * @return    WIFI_API_RESULT                            - Status
 * @retval    WIFI_API_RESULT_SUCCESS                    - Success
 * @retval    WIFI_API_RESULT_NOT_INITIALIZED            - Not initialized
 * @retval    WIFI_API_RESULT_OPERATION_NOT_SUPPORTED    - Operation not supported
 * @retval    WIFI_API_RESULT_NULL_PARAM                 - Null param
 * @retval    WIFI_API_RESULT_INVALID_PARAM              - Invalid param
 * @retval    WIFI_API_RESULT_READ_WRITE_FAILED          - flash operation failed
 *
 * @pre  mfr_init() should be called before calling this API. If this precondition is not met, the API will return WIFI_API_RESULT_NOT_INITIALIZED.
 * @warning  This API is NOT thread safe. Caller shall handle the concurrency
 * @see  WIFI_GetCredentials()
 *
 */
WIFI_API_RESULT WIFI_SetCredentials(WIFI_DATA *pData)
{
    if (!isLibraryInitialized()) {
        mfrlib_log("isLibraryInitialized not initialized\n");
        return WIFI_API_RESULT_NOT_INITIALIZED;
    }

    if (NULL == pData) {
        return WIFI_API_RESULT_NULL_PARAM;
    }

    if ((pData->cSSID[0] == '\0') || (pData->cPassword[0] == '\0')) {
        return WIFI_API_RESULT_INVALID_PARAM;
    }

    return WIFI_API_RESULT_OPERATION_NOT_SUPPORTED;
}

/**
 * @brief Clears the wifi credentials saved in the  MFR persistance storage @see WIFI_DATA
 *
 * @return    WIFI_API_RESULT                     - Status
 * @retval    WIFI_API_RESULT_SUCCESS             - Success
 * @retval    WIFI_API_RESULT_NOT_INITIALIZED     - Not initialized
 * @retval    WIFI_API_RESULT_OPERATION_NOT_SUPPORTED    - Operation not supported
 * @retval    WIFI_API_RESULT_READ_WRITE_FAILED   - flash operation failed
 *
 * @pre  mfr_init() should be called before calling this API. If this precondition is not met, the API will return WIFI_API_RESULT_NOT_INITIALIZED.
 * @warning  This API is NOT thread safe. Caller shall handle the concurrency
 *
 */
WIFI_API_RESULT WIFI_EraseAllData(void)
{
    if (!isLibraryInitialized()) {
        mfrlib_log("isLibraryInitialized not initialized\n");
        return WIFI_API_RESULT_NOT_INITIALIZED;
    }

    return WIFI_API_RESULT_OPERATION_NOT_SUPPORTED;
}
