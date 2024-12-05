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
#include <mfrMgr.h>
#include <mfrTypes.h>
#include <stdlib.h>
#include <string.h>
#include "mfr_wifi_types.h"
#include "mfr_wifi_api.h"
#define MAX_BUF_LEN 255
#define SIZE 50
#define MAC_ADDRESS_SIZE 12
#define SERIAL_MAX_SIZE 16
#define MAX_COMMAND_SIZE 100

//#define DEBUG

const char defaultManufacturer[] = "UNKNOWN MANUFACTURER";
const char defaultManufacturerOUI[] = "B827EB";
const char defaultModelName[] = "RASPBERRYPI_3";
const char defaultDescription[] = "RASPBERRYPI_3 mediaclient";
const char defaultProductClass[] = "RDK";
const char defaultSerialNumber[] = "0000000000000000";
const char defaultHardwareVersion[] = "RASPBERRYPI_3_V01";
const char defaultSoftwareVersion[] = "2.0";
const char defaultMacAddress[] = "00:00:00:00:00:00";
const char defaultBluetoothMacAddress[] = "00:00:00:00:00:00";
const char defaultWifiMacAddress[] = "00:00:00:00:00:00";

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

void mfrFreeBuffer(char *buf)
{
    if (buf)
        free(buf);
}

mfrError_t mfrGetSerializedData(mfrSerializedType_t param, mfrSerializedData_t *data)
{
    char cmd[MAX_COMMAND_SIZE];
    char buffer[MAX_BUF_LEN];
    FILE *fp = NULL;
    mfrError_t ret = mfrERR_NONE;

    if (!data)
        return mfrERR_INVALID_PARAM;
    data->freeBuf = mfrFreeBuffer;

    switch (param)
    {
    case mfrSERIALIZED_TYPE_MANUFACTURER:
        data->buf = (char *)malloc(sizeof(char) * MAX_BUF_LEN);
        memset(cmd, 0, sizeof(char) * 100);
        memset(data->buf, '\0', sizeof(char) * MAX_BUF_LEN);
        memset(buffer, 0, MAX_BUF_LEN);

        /* retrieving tag MANUFACTURE from /etc/device.properties */
        sprintf(cmd, "cat /etc/device.properties | grep MANUFACTURE | sed -e 's/.*=//g'");
        if ((fp = popen(cmd, "r")) == NULL)
        {
            mfrlib_log("popen failed.");
            strcpy(data->buf, defaultManufacturer);
            data->bufLen = strlen(data->buf);
        }
        if (fp)
        {
            fgets(buffer, sizeof(buffer), fp);
            strncpy(data->buf, buffer, strlen(buffer) - 1);
            data->bufLen = strlen(data->buf);
            pclose(fp);
        }
        mfrlib_log("Manufacturer= %s\t len=%d\n", data->buf, data->bufLen);
        break;
    /* unique identifier of the Manufacturer :: we are using the first 6 chars of the mac address */
    case mfrSERIALIZED_TYPE_MANUFACTUREROUI:
        data->buf = (char *)malloc(sizeof(char) * MAX_BUF_LEN);
        memset(data->buf, '\0', sizeof(char) * MAX_BUF_LEN);
        memset(cmd, 0, sizeof(char) * 100);
        memset(buffer, 0, MAX_BUF_LEN);

        sprintf(cmd, "ifconfig | grep `ifconfig -a | sed 's/[ \t].*//;/^\(lo\|\)$/d' | head -n1` | tr -s ' ' | cut -d ' ' -f5 | sed -e 's/://g'");

        if ((fp = popen(cmd, "r")) == NULL)
        {
            mfrlib_log("popen failed.");
            strcpy(data->buf, defaultManufacturerOUI);
            data->bufLen = strlen(data->buf);
        }
        if (fp)
        {
            fgets(buffer, sizeof(buffer), fp);
            strncpy(data->buf, buffer, 6);
            data->bufLen = 6;
            pclose(fp);
        }
        mfrlib_log("Manufacturer OUI= %s\t len=%d\n", data->buf, data->bufLen);
        break;
    case mfrSERIALIZED_TYPE_MODELNAME:
        data->buf = (char *)malloc(sizeof(char) * MAX_BUF_LEN);
        memset(data->buf, '\0', sizeof(char) * MAX_BUF_LEN);
        memset(cmd, 0, sizeof(char) * 100);
        memset(buffer, 0, MAX_BUF_LEN);
        /* retrieving tag MODEL_NUM from /etc/device.properties */
        sprintf(cmd, "cat /etc/device.properties | grep MODEL_NUM | sed -e 's/.*=//g'");

        if ((fp = popen(cmd, "r")) == NULL)
        {
            mfrlib_log("popen failed.");
            strcpy(data->buf, defaultModelName);
            data->bufLen = strlen(data->buf);
        }
        if (fp)
        {
            fgets(buffer, sizeof(buffer), fp);
            strncpy(data->buf, buffer, strlen(buffer) - 1);
            data->bufLen = strlen(data->buf);
            pclose(fp);
        }
        mfrlib_log("Model Name= %s\t len=%d\n", data->buf, data->bufLen);
        break;
    case mfrSERIALIZED_TYPE_DESCRIPTION:
        data->buf = (char *)malloc(sizeof(char) * MAX_BUF_LEN);
        memset(data->buf, '\0', sizeof(char) * MAX_BUF_LEN);
        memset(cmd, 0, sizeof(char) * 100);
        memset(buffer, 0, MAX_BUF_LEN);
        /* retrieving tag DEVICE_TYPE from /etc/device.properties */
        sprintf(cmd, "cat /etc/device.properties | grep DEVICE_TYPE | sed -e 's/.*=//g'");

        if ((fp = popen(cmd, "r")) == NULL)
        {
            mfrlib_log("popen failed.");
            strcpy(data->buf, defaultDescription);
            data->bufLen = strlen(data->buf);
        }
        if (fp)
        {
            fgets(buffer, sizeof(buffer), fp);
            sprintf(data->buf, "%s ", defaultModelName);
            strncat(data->buf, buffer, strlen(buffer) - 1);
            data->bufLen = strlen(data->buf);
            pclose(fp);
        }
        mfrlib_log("DESCRIPTION= %s\t len=%d\n", data->buf, data->bufLen);
        break;
    case mfrSERIALIZED_TYPE_PRODUCTCLASS:
        data->buf = (char *)malloc(sizeof(char) * MAX_BUF_LEN);
        memset(data->buf, '\0', sizeof(char) * MAX_BUF_LEN);
        memset(cmd, 0, sizeof(char) * 100);
        memset(buffer, 0, MAX_BUF_LEN);
        /* retrieving tag DEVICE_NAME from /etc/device.properties */
        sprintf(cmd, "cat /etc/device.properties | grep DEVICE_NAME | sed -e 's/.*=//g'");

        if ((fp = popen(cmd, "r")) == NULL)
        {
            mfrlib_log("popen failed.");
            strcpy(data->buf, defaultProductClass);
            data->bufLen = strlen(data->buf);
        }
        if (fp)
        {
            fgets(buffer, sizeof(buffer), fp);
            strncpy(data->buf, buffer, strlen(buffer) - 1);
            data->bufLen = strlen(data->buf);
            pclose(fp);
        }
        mfrlib_log("PRODUCT CLASS= %s\t len=%d\n", data->buf, data->bufLen);
        break;
    case mfrSERIALIZED_TYPE_SERIALNUMBER:
    case mfrSERIALIZED_TYPE_MANUFACTURING_SERIALNUMBER:
        data->buf = (char *)malloc(sizeof(char) * MAX_BUF_LEN);
        memset(data->buf, '\0', sizeof(char) * MAX_BUF_LEN);
        memset(cmd, 0, sizeof(char) * 100);
        memset(buffer, 0, MAX_BUF_LEN);
        /* retrieving tag Serial from /proc/cpuinfo */
        sprintf(cmd, "cat /proc/cpuinfo | grep Serial | sed -e 's/.*: //g'");
        if ((fp = popen(cmd, "r")) == NULL)
        {
            mfrlib_log("popen failed.");
            strcpy(data->buf, defaultSerialNumber);
            data->bufLen = strlen(data->buf);
        }
        if (fp)
        {
            fgets(buffer, sizeof(buffer), fp);
            strncpy(data->buf, buffer, SERIAL_MAX_SIZE);
            data->bufLen = strlen(data->buf);
            pclose(fp);
        }
        mfrlib_log("Serial Number =  %s\t len=%d\n", data->buf, data->bufLen);
        break;
    case mfrSERIALIZED_TYPE_HARDWAREVERSION:
        data->buf = (char *)malloc(sizeof(char) * MAX_BUF_LEN);
        memset(data->buf, '\0', sizeof(char) * MAX_BUF_LEN);
        memset(cmd, 0, sizeof(char) * 100);
        memset(buffer, 0, MAX_BUF_LEN);
        /* retrieving tag Revision from /proc/cpuinfo */
        sprintf(cmd, "cat /proc/cpuinfo | grep Revision | sed -e 's/.*: //g'");
        if ((fp = popen(cmd, "r")) == NULL)
        {
            mfrlib_log("popen failed.");
            strcpy(data->buf, defaultHardwareVersion);
            data->bufLen = strlen(data->buf);
        }
        if (fp)
        {
            fgets(buffer, sizeof(buffer), fp);
            strncpy(data->buf, buffer, strlen(buffer) - 1);
            data->bufLen = strlen(data->buf);
            pclose(fp);
        }
        mfrlib_log("Hardware Version= %s\t len=%d\n", data->buf, data->bufLen);
        break;
    case mfrSERIALIZED_TYPE_SOFTWAREVERSION:
        data->buf = (char *)malloc(sizeof(char) * MAX_BUF_LEN);
        memset(data->buf, '\0', sizeof(char) * MAX_BUF_LEN);
        memset(cmd, 0, sizeof(char) * 100);
        memset(buffer, 0, MAX_BUF_LEN);
        /* retrieving tag BUILD_VERSION from /etc/device.properties */
        sprintf(cmd, "cat /etc/device.properties | grep BUILD_VERSION | sed -e 's/.*=//g'");
        if ((fp = popen(cmd, "r")) == NULL)
        {
            mfrlib_log("popen failed.");
            strcpy(data->buf, defaultSoftwareVersion);
            data->bufLen = strlen(data->buf);
        }
        if (fp)
        {
            fgets(buffer, sizeof(buffer), fp);
            if (strlen(buffer) > 1) {
            strncpy(data->buf, buffer, strlen(buffer) - 1);
            } else {
                strcpy(data->buf, defaultSoftwareVersion);
            }
            data->bufLen = strlen(data->buf);
            pclose(fp);
        }
        mfrlib_log("Build Version =  %s\t len=%d\n", data->buf, data->bufLen);
        break;
    case mfrSERIALIZED_TYPE_PROVISIONINGCODE:
    case mfrSERIALIZED_TYPE_FIRSTUSEDATE:
    case mfrSERIALIZED_TYPE_MOCAMAC:
    case mfrSERIALIZED_TYPE_HDMIHDCP:
    case mfrSERIALIZED_TYPE_PDRIVERSION:
        data->buf = (char *)malloc(sizeof(char) * MAX_BUF_LEN);
        memset(data->buf, '\0', sizeof(char) * MAX_BUF_LEN);
        strcpy(data->buf, "Not implemented");
        break;
    case mfrSERIALIZED_TYPE_MAX:
        data->buf = (char *)malloc(sizeof(char) * MAX_BUF_LEN);
        strcpy(data->buf, "XXXX");
        data->bufLen = strlen(data->buf);
        break;
    case mfrSERIALIZED_TYPE_DEVICEMAC:
        data->buf = (char *)malloc(sizeof(char) * MAX_BUF_LEN);
        memset(data->buf, '\0', sizeof(char) * MAX_BUF_LEN);
        memset(cmd, 0, sizeof(char) * 100);
        memset(buffer, 0, MAX_BUF_LEN);

        sprintf(cmd, "ifconfig | grep eth0 | awk '{print $5}'");
        if ((fp = popen(cmd, "r")) == NULL)
        {
            mfrlib_log("popen failed.");
            strcpy(data->buf, defaultMacAddress);
            data->bufLen = strlen(data->buf);
        }
        if (fp)
        {
            fgets(buffer, sizeof(buffer), fp);
            if (strlen(buffer) > 1) {
                buffer[strcspn(buffer, "\n")] = 0;
                strncpy(data->buf, buffer, MAX_BUF_LEN - 1);
                data->buf[MAX_BUF_LEN - 1] = '\0';
                } else {
                strcpy(data->buf, defaultMacAddress);
            }
            data->bufLen = strlen(data->buf);
            pclose(fp);
        }
        mfrlib_log("MAC Address = %s\t len=%d\n", data->buf, data->bufLen);
        break;
    case mfrSERIALIZED_TYPE_BLUETOOTHMAC:
        data->buf = (char *)malloc(sizeof(char) * MAX_BUF_LEN);
        memset(data->buf, '\0', sizeof(char) * MAX_BUF_LEN);
        memset(cmd, 0, sizeof(char) * 100);
        memset(buffer, 0, MAX_BUF_LEN);

        sprintf(cmd, "hciconfig | grep \"BD Address\" | awk '{ print $3}'");

        if ((fp = popen(cmd, "r")) == NULL)
        {
            mfrlib_log("popen failed.");
            strcpy(data->buf, defaultBluetoothMacAddress);
            data->bufLen = strlen(data->buf);
        }
        if (fp)
        {
            fgets(buffer, sizeof(buffer), fp);
            if (strlen(buffer) > 1) {
                buffer[strcspn(buffer, "\n")] = 0;
                strncpy(data->buf, buffer, MAX_BUF_LEN - 1);
                data->buf[MAX_BUF_LEN - 1] = '\0';
            } else {
                strcpy(data->buf, defaultBluetoothMacAddress);
            }
            data->bufLen = strlen(data->buf);
            pclose(fp);
        }
        mfrlib_log("Bluetooth MAC Address = %s\t len=%d\n", data->buf, data->bufLen);
        break;
    case mfrSERIALIZED_TYPE_WIFIMAC:
        data->buf = (char *)malloc(sizeof(char) * MAX_BUF_LEN);
        memset(data->buf, '\0', sizeof(char) * MAX_BUF_LEN);
        memset(cmd, 0, sizeof(char) * 100);
        memset(buffer, 0, MAX_BUF_LEN);

        sprintf(cmd, "ifconfig | grep wlan0 | awk '{print $5}' ");

        if ((fp = popen(cmd, "r")) == NULL)
        {
            mfrlib_log("popen failed.");
            strcpy(data->buf, defaultWifiMacAddress);
            data->bufLen = strlen(data->buf);
        }
        if (fp)
        {
            fgets(buffer, sizeof(buffer), fp);
            if (strlen(buffer) > 1) {
                buffer[strcspn(buffer, "\n")] = 0;
                strncpy(data->buf, buffer, MAX_BUF_LEN - 1);
                data->buf[MAX_BUF_LEN - 1] = '\0';
            } else {
                strcpy(data->buf, defaultWifiMacAddress);
            }
            data->bufLen = strlen(data->buf);
            pclose(fp);
        }
        mfrlib_log("Wifi MAC Address = %s\t len=%d\n", data->buf, data->bufLen);
        break;
    default:
        data->buf = (char *)malloc(sizeof(char) * MAX_BUF_LEN);
        memset(data->buf, '\0', sizeof(char) * MAX_BUF_LEN);
        data->bufLen = strlen(data->buf);
        break;
    }
    return mfrERR_NONE;
}
WIFI_API_RESULT WIFI_GetCredentials(WIFI_DATA *pData)
{
   if (pData == NULL) 
   {
        return WIFI_API_RESULT_NULL_PARAM;
   }
 
   return WIFI_API_RESULT_OPERATION_NOT_SUPPORTED;
}
 
WIFI_API_RESULT WIFI_SetCredentials(WIFI_DATA *pData)
{
    if (pData == NULL)
    {
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
