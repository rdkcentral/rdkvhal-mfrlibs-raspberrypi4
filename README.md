# rdkvhal-mfrlibs-raspberrypi4
RDKV IARMMGRS MFR HAL Implementation of RPi4

The implementation adheres to the IARMMGRS MFR HAL Interface (HALIF) requirements.

### Serialized Data Support

This repository is an STB-oriented MFR HAL implementation. Support for `PANEL_SERIALIZATION_TYPES` is intentionally out of scope and those values are not accepted by the serialized-type validator on this target.

The current implementation of `mfrGetSerializedData()` in `mfrlibs_rpi.c` supports the following `mfrSerializedType_t` values.

| Enum value | Implementation | Backing source |
| --- | --- | --- |
| `mfrSERIALIZED_TYPE_MANUFACTURER` | Reads the `MANUFACTURE` key | `/etc/device.properties` |
| `mfrSERIALIZED_TYPE_MANUFACTUREROUI` | Uses the first 6 hex characters of `eth0` MAC | `eth0` interface MAC |
| `mfrSERIALIZED_TYPE_MODELNAME` | Reads the `DEVICE_NAME` key | `/etc/device.properties` |
| `mfrSERIALIZED_TYPE_PROVISIONED_MODELNAME` | Uses the same implementation as `mfrSERIALIZED_TYPE_MODELNAME` | `/etc/device.properties` |
| `mfrSERIALIZED_TYPE_PMI` | Uses the same implementation as `mfrSERIALIZED_TYPE_MODELNAME` | `/etc/device.properties` |
| `mfrSERIALIZED_TYPE_DESCRIPTION` | Returns the built-in constant `RaspberryPi RDKV Reference Device` | Constant in code |
| `mfrSERIALIZED_TYPE_PRODUCTCLASS` | Returns the built-in constant `RDKV` | Constant in code |
| `mfrSERIALIZED_TYPE_SERIALNUMBER` | Reads the `Serial` field | `/proc/cpuinfo` |
| `mfrSERIALIZED_TYPE_MANUFACTURING_SERIALNUMBER` | Same implementation as `mfrSERIALIZED_TYPE_SERIALNUMBER` | `/proc/cpuinfo` |
| `mfrSERIALIZED_TYPE_HARDWAREVERSION` | Reads the `Revision` field | `/proc/cpuinfo` |
| `mfrSERIALIZED_TYPE_FIRSTUSEDATE` | Reads the file birth time and formats it as `YYYY-MM-DD` | `/boot/first_use_date.txt` |
| `mfrSERIALIZED_TYPE_DEVICEMAC` | Reads the MAC address of `eth0` | `eth0` interface MAC |
| `mfrSERIALIZED_TYPE_ETHERNETMAC` | Same implementation as `mfrSERIALIZED_TYPE_DEVICEMAC` | `eth0` interface MAC |
| `mfrSERIALIZED_TYPE_ESTBMAC` | Same implementation as `mfrSERIALIZED_TYPE_DEVICEMAC` | `eth0` interface MAC |
| `mfrSERIALIZED_TYPE_WIFIMAC` | Reads the MAC address of `wlan0` | `wlan0` interface MAC |
| `mfrSERIALIZED_TYPE_SOFTWAREVERSION` | Returns the built-in constant `2.0` | Constant in code |
| `mfrSERIALIZED_TYPE_MOCAMAC` | Reads `MOCA_INTERFACE` and then that interface MAC | `/etc/device.properties` and resolved interface MAC |
| `mfrSERIALIZED_TYPE_BLUETOOTHMAC` | Parses Bluetooth address from `hciconfig -a` output | Shell command output |
| `mfrSERIALIZED_TYPE_HWID` | Reads the `Revision` field | `/proc/cpuinfo` |
| `mfrSERIALIZED_TYPE_MODELNUMBER` | Same implementation as `mfrSERIALIZED_TYPE_HWID` | `/proc/cpuinfo` |
| `mfrSERIALIZED_TYPE_SOC_ID` | Reads chip details from devicetree | `/proc/device-tree/compatible` |
| `mfrSERIALIZED_TYPE_IMAGENAME` | Reads the `imagename` key | `/version.txt` |
| `mfrSERIALIZED_TYPE_IMAGETYPE` | Returns the built-in constant `PCI` | Constant in code |
| `mfrSERIALIZED_TYPE_BLVERSION` | Reads the first 7 bytes from the bootloader version node | `/sys/firmware/devicetree/base/chosen/bootloader/version` |

The following base enum values are accepted by the type validator range but currently return `mfrERR_OPERATION_NOT_SUPPORTED` from `mfrGetSerializedData()`:

- `mfrSERIALIZED_TYPE_PROVISIONINGCODE`
- `mfrSERIALIZED_TYPE_HDMIHDCP`
- `mfrSERIALIZED_TYPE_PDRIVERSION`
- `mfrSERIALIZED_TYPE_WPSPIN`
- `mfrSERIALIZED_TYPE_RF4CEMAC`
- `mfrSERIALIZED_TYPE_REGION`
- `mfrSERIALIZED_TYPE_BDRIVERSION`
- `mfrSERIALIZED_TYPE_LED_WHITE_LEVEL`
- `mfrSERIALIZED_TYPE_LED_PATTERN`
- `mfrSERIALIZED_TYPE_SKYMODELNAME`
- `mfrSERIALIZED_TYPE_DE_SERIAL_PREFIX`

Additional enum values are also not supported on this platform:

- `mfrSERIALIZED_TYPE_MAX` is a sentinel and not a readable type.
- All `PANEL_SERIALIZATION_TYPES` values are out of scope for this STB implementation and are rejected as invalid parameters.

`mfrSetSerializedData()` is currently not implemented for any serialized type and always returns `mfrERR_OPERATION_NOT_SUPPORTED`.

### To enable debug logs from this HAL module

Create `/etc/debug.ini` and add the following line
```
LOG.RDK.MFRMGR = LOG DEBUG INFO ERROR
```

### Related Repositories

- **HAL Header Repository**: [iarmmgrs/mfr/include](https://github.com/rdkcentral/iarmmgrs/tree/main/mfr/include) [v1.1.10](https://github.com/rdkcentral/iarmmgrs/releases/tag/1.1.10)
- **HAL Test Suite Repository**: [rdk-halif-test-device_settings](https://github.com/rdkcentral/rdk-halif-test-mfr)
