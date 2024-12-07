/*
 * If not stated otherwise in this file or this component's LICENSE file the
 * following copyright and licenses apply:
 *
 * Copyright 2024 RDK Management
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
#include <unistd.h>

#include "mfrTypes.h"

/* Keep order matching with mfrSerializedType_t from mfrTypes.h */
const char* mfrSerializedTypeString[] = {
    "manufacturer",
    "manufactureroui",
    "modelname",
    "description",
    "productclass",
    "serialnumber",
    "hardwareversion",
    "softwareversion",
    "provisioningcode",
    "firstusedate",
    "devicemac",
    "mocamac",
    "hdmihdcp",
    /* *** */
    "pdriversion",
    "wifimac",
    "bluetoothmac",
    "wpspin",
    "manufacturingserialnumber",
    "ethernetmac",
    "estbmac",
    "rf4cemac",
    /* *** */
    "provisionedmodelname",
    "pmi",
    "hwid",
    "modelnumber",
    /* boot data */
    "socid",
    "imagename",
    "imagetype",
    "blversion",
    /* provisional data */
    "region",
    /* other data */
    "bdriversion",
    /* led data */
    "ledwhitelevel",
    "ledpattern",
    NULL
};

mfrSerializedType_t getmfrSerializedTypeFromString(char *pString)
{
    mfrSerializedType_t i;
    if (!pString) {
        return mfrSERIALIZED_TYPE_MAX;
    }
    for (i = mfrSERIALIZED_TYPE_MANUFACTURER; mfrSerializedTypeString[i]; i++) {
        if (strcmp(pString, mfrSerializedTypeString[i]) == 0) {
            break;
        }
    }
    return i;
}

void showUsage(const char *progName)
{
    printf("Usage: %s [-r serializedTypeString ]\n"
           "\t-a: Read each type of serialized data one by one.\n"
           "\t-r serializedTypeString: Read the serialized data of the given type\n"
           "\t\t type: ", progName);
    for (mfrSerializedType_t i = mfrSERIALIZED_TYPE_MANUFACTURER; mfrSerializedTypeString[i]; i++) {
        printf("%s ", mfrSerializedTypeString[i]);
        if (i && !(i % 5)) {
            printf("\n\t\t      ");
        }
    }
    printf("\n\t\tNote: 'Type' arguments are case sensitive.\n");
}

void printSerializedData(mfrSerializedType_t type)
{
    mfrSerializedData_t mfrSerializedData = {0};
    mfrError_t retVal = mfrGetSerializedData(type, &mfrSerializedData);
    if (retVal == mfrERR_NONE) {
        printf("mfrSerializedData.buf    :'%s'\n", mfrSerializedData.buf);
        printf("mfrSerializedData.bufLen : %d\n", mfrSerializedData.bufLen);
        if (mfrSerializedData.freeBuf) {
            mfrSerializedData.freeBuf(mfrSerializedData.buf);
        } else {
            printf("mfrSerializedData.freeBuf is NULL\n");
        }
    } else {
        printf("mfrGetSerializedData failed for '%s', error code '%x'\n", mfrSerializedTypeString[type], retVal);
    }
}

int main(int argc, char **argv) {
    int c;
    if (argc >= 2) {
        while ((c = getopt(argc, argv, "r:a")) != -1) {
            switch (c) {
                case 'r':
                    if (optarg) {
                        printSerializedData(getmfrSerializedTypeFromString(optarg));
                    } else {
                        showUsage(argv[0]);
                        return -1;
                    }
                    break;
                case 'a':
                    for (mfrSerializedType_t i = mfrSERIALIZED_TYPE_MANUFACTURER; mfrSerializedTypeString[i]; i++) {
                        printSerializedData(i);
                        printf("\n");
                    }
                    break;
                default:
                    showUsage(argv[0]);
                    return -1;
            }
        }
    } else {
        showUsage(argv[0]);
        return -1;
    }
    return 0;
}
