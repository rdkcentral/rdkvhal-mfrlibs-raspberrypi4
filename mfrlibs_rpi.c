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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <bluetooth/hci.h>
#include <bluetooth/bluetooth.h>

#include <mfrMgr.h>
#include <mfrTypes.h>
#include <mfr_wifi_types.h>
#include <mfr_wifi_api.h>

#define MAX_BUF_LEN 255
#define MAC_ADDRESS_SIZE 32

const char defaultDescription[] = "RaspberryPi RDKV Reference Device";
const char defaultProductClass[] = "RDKV";
const char defaultSoftwareVersion[] = "2.0";

void mfrlib_log(const char *format, ...)
{
    int total = 0;
    va_list args;
    int buf_index;

#ifdef DEBUG
    va_start(args, format);
    // log to console
    total = vfprintf(stdout, format, args);
    fflush(stdout);
    va_end(args);
#endif
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
    if (fp == NULL) {
        mfrlib_log("getValueFromVersionFile fopen failed for /version.txt\n");
        return retValue;
    }

    while ((read = getline(&line, &len, fp)) != -1) {
        if (strncmp(line, key, keyLen) == 0 && line[keyLen] == separator) {
            char *value = line + keyLen + 1;
            while (*value == ' ' && *value != '\0') value++;
            strncpy(valueOut, value, maxLen - 1);
            valueOut[maxLen - 1] = '\0'; // Ensure null-termination
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
    int sock;
    struct hci_dev_info di;

    if (!bdAddress || maxLen < 18) {
        mfrlib_log("getBDAddress invalid input.\n");
        return -1;
    }

    sock = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
    if (sock < 0) {
        mfrlib_log("getBDAddress socket creation failed\n");
        return -1;
    }

    memset(&di, 0, sizeof(di));
    di.dev_id = 0;
    if (ioctl(sock, HCIGETDEVINFO, (void *)&di) < 0) {
        mfrlib_log("getBDAddress ioctl failed\n");
        close(sock);
        return -1;
    }

    close(sock);
    snprintf(bdAddress, maxLen, "%02X:%02X:%02X:%02X:%02X:%02X",
             di.bdaddr.b[5], di.bdaddr.b[4], di.bdaddr.b[3],
             di.bdaddr.b[2], di.bdaddr.b[1], di.bdaddr.b[0]);

    return 0;
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
        if (fp == NULL) {
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
                strncpy(valueOut, value, size - 1);
                valueOut[size - 1] = '\0';
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
        if (fp == NULL) {
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
                strncpy(valueOut, value, size - 1);
                valueOut[size - 1] = '\0';
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
 * @note Refer https://github.com/rdk-e/iarmmgrs/blob/main/mfr/include/mfrTypes.h#L205
 */
bool isValidMfrSerializedType(mfrSerializedType_t param) {
    // Check if param is within the valid range of mfrSerializedType_t
    if ((param >= mfrSERIALIZED_TYPE_MANUFACTURER && param < mfrSERIALIZED_TYPE_MAX) ||
#ifdef PANEL_SERIALIZATION_TYPES
        (param >= mfrSERIALIZED_TYPE_COREBOARD_SERIALNUMBER && param <= mfrSERIALIZED_TYPE_PANEL_MAX) ||
#endif /* PANEL_SERIALIZATION_TYPES */
    ) {
        return true;
    }
    return false;
}

mfrError_t mfrGetSerializedData(mfrSerializedType_t param, mfrSerializedData_t *data)
{
    char cmd[MAX_BUF_LEN] = {0};
    char buffer[MAX_BUF_LEN] = {0};
    FILE *fp = NULL;
    mfrError_t ret = mfrERR_NONE;

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
                mfrlib_log("Manufacturer= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                data->freeBuf = NULL;
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
                mfrlib_log("Manufacturer OUI= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                data->freeBuf = NULL;
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
                mfrlib_log("Model Name= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                data->freeBuf = NULL;
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
                mfrlib_log("Serial Number= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                data->freeBuf = NULL;
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
                mfrlib_log("Hardware Version= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                data->freeBuf = NULL;
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
                mfrlib_log("Device MAC= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                data->freeBuf = NULL;
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
                mfrlib_log("WiFi MAC= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                data->freeBuf = NULL;
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
                    mfrlib_log("MOCA MAC= '%s', len=%d\n", data->buf, data->bufLen);
                } else {
                    mfrFreeBuffer(data->buf);
                    data->freeBuf = NULL;
                    mfrlib_log("getInterfaceMACString failed, return mfrERR_FLASH_READ_FAILED.\n");
                    ret = mfrERR_FLASH_READ_FAILED;
                }
            } else {
                mfrFreeBuffer(data->buf);
                data->freeBuf = NULL;
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
                mfrlib_log("Bluetooth MAC= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                data->freeBuf = NULL;
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
                mfrlib_log("HWID= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                data->freeBuf = NULL;
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
                mfrlib_log("SOC ID= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                data->freeBuf = NULL;
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
                mfrlib_log("Image Name= '%s', len=%d\n", data->buf, data->bufLen);
            } else {
                mfrFreeBuffer(data->buf);
                data->freeBuf = NULL;
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

WIFI_API_RESULT WIFI_GetCredentials(WIFI_DATA *pData)
{
   if (pData == NULL) {
        return WIFI_API_RESULT_NULL_PARAM;
   }

   return WIFI_API_RESULT_OPERATION_NOT_SUPPORTED;
}

WIFI_API_RESULT WIFI_SetCredentials(WIFI_DATA *pData)
{
    if (pData == NULL) {
        return WIFI_API_RESULT_NULL_PARAM;
    }

    return WIFI_API_RESULT_OPERATION_NOT_SUPPORTED;
}
mfrError_t mfrDeletePDRI()
{
    return mfrERR_NONE;
}

mfrError_t mfrScrubAllBanks()
{
    return mfrERR_NONE;
}

mfrError_t mfrWriteImage(const char *str, const char *str1,
                         mfrImageType_t imageType, mfrUpgradeStatusNotify_t upgradeStatus)
{
    return mfrERR_NONE;
}

mfrError_t mfr_init()
{
    return mfrERR_NONE;
}
