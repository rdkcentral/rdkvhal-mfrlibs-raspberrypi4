# rdkvhal-mfrlibs-raspberrypi4
RDKV IARMMGRS MFR HAL Implementation of RPi4

The implementation adheres to the IARMMGRS MFR HAL Interface (HALIF) requirements.

### Building with CMake

This is a Linux-targeted CMake project. Building requires CMake 3.16 or newer,
C and C++ compilers (C99 and C++17), pkg-config, libarchive development files,
and the RDK MFR HAL and IARM headers.

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib
cmake --build build
DESTDIR="$PWD/staging" cmake --install build
```

The build produces `libRDKMfrLib.so`, `mfrHalUtility`, and `FlashApp`. The staged
installation above places the library under `staging/usr/lib` and both
executables under `staging/usr/bin`. The library retains the previous
`libRDKMfrLib.so.0` SONAME and `0.0.0` library version.
`FlashApp` keeps its blocking CLI interface. By default, the HAL also builds
the C-callable `NativeFlashBridge.cpp` and `WicStreamFlasher.cpp`, so
`mfrWriteImage()` accepts work asynchronously without compiling the C HAL as C++.
Both native consumers link libarchive, threads, and `stdc++fs` when needed.
The shell wrapper is not installed by the build, matching the previous behavior.

| CMake option | Default | Purpose |
| --- | --- | --- |
| `ENABLE_THERMAL_PROTECTION` | `OFF` | Includes the HAL thermal protection functions. |
| `ENABLE_SINGLE_INSTANCE_LOCK` | `OFF` | Enables the HAL single-instance lock. |
| `ENABLE_NATIVE_FLASHER` | `ON` | Enables asynchronous native OTA flashing through `mfrWriteImage()`. `OFF` leaves that HAL API unsupported; the CLI is still built. |
| `RDKMFRLIBS_INCLUDE_DIRS` | RDK header directories under the compiler sysroot's `/usr/include` | Semicolon-separated HAL and IARM include directories. |
| `RDKMFRLIBS_LIBS` | Empty | Semicolon-separated additional HAL libraries. |

For example, enable both optional HAL features with
`-DENABLE_THERMAL_PROTECTION=ON -DENABLE_SINGLE_INSTANCE_LOCK=ON`.
The HAL lock option does not control `FlashApp`'s own lock.
The native HAL worker uses the same flash lock as the CLI, preventing concurrent
updates between the two entry points.

`mfrWriteImage()` returns an error without callbacks if validation, reservation,
or worker creation fails. Success means the request was accepted, not that the
update finished. The owned worker sends `STARTED` notifications at the requested
interval, then exactly one `COMPLETED` or `ABORTED` notification; post-start
failures are reported through the callback rather than changing the API return.
An interval of zero delivers only the final notification. Keep callback code
and caller-owned callback resources alive until completion. Callbacks must not
throw C++ exceptions. `mfr_term()` waits for the worker and all callbacks before
releasing library resources; calling it from a flash callback is rejected to
avoid self-join deadlocks. Do not unload the library before termination completes.

For cross-compilation, initialize the target SDK environment and supply its
CMake toolchain file:

```sh
cmake -S . -B build \
    -DCMAKE_TOOLCHAIN_FILE=/path/to/sdk/toolchain.cmake \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_INSTALL_LIBDIR=lib
cmake --build build
```

The toolchain should configure `CMAKE_SYSROOT`, the target C/C++ compilers, and
target dependency lookup. Ensure pkg-config uses the target's libarchive
metadata, not the build host's. Override `RDKMFRLIBS_INCLUDE_DIRS` if the SDK
stores the RDK headers elsewhere. Filesystem checks link but do not run target
executables; older GCC toolchains automatically link `stdc++fs` when needed.

CMake replaces Autotools: `autoreconf`, `configure`, and Libtool `.la` files
are no longer provided. External Yocto recipes must switch from
`inherit autotools` to `inherit cmake pkgconfig` and add `libarchive` to
`DEPENDS`. Replace `--enable-thermalprotection` with
`-DENABLE_THERMAL_PROTECTION=ON` and `--enable-single-instance-lock` with
`-DENABLE_SINGLE_INSTANCE_LOCK=ON`.

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
