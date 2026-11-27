# Native Flash Application

This document describes the native OTA flasher implemented by `FlashApp.cpp`,
`FlashProcessor.hpp`, and `FlashConfig.hpp`. The repository does not currently
contain a `FlashConfig.cpp`; its configuration is defined in the header.

## Building

The repository uses CMake to build the native flasher alongside the HAL
library and utility. See [Building with CMake](README.md#building-with-cmake)
for prerequisites, installation, build options, and cross-compilation.

After configuring the build directory, build only the flasher with:

```sh
cmake --build build --target FlashApp
```

The executable is `build/FlashApp` with a single-configuration generator.
It links libarchive and thread support, requires C++17 filesystem operations,
and is installed to the configured binary directory.

## Invocation

The entry point accepts either an OTA path directly or a directory and file
name:

```text
FlashApp <absolute path to OTA file>
FlashApp <directory path> <OTA file name>
```

Invalid argument counts print both usage forms and return status `1`. The
application installs handlers for `SIGINT`, `SIGTERM`, and `SIGHUP`, creates a
`NativeFlashProcessor` for the supplied path, and returns `0` when
`flash()` succeeds or `1` when it fails. A handled signal exits with that
signal number.

## Update Workflow

`NativeFlashProcessor::flash()` performs the following operations:

1. Acquires an exclusive, non-blocking lock at `/var/run/flash_app.lock`. A
   second instance reports that an update is active and aborts.
2. Checks that the input path exists and applies the configured CPU priority,
   I/O priority, and CPU affinity settings.
3. If the file has a gzip signature, creates `/opt/ota/wicimage`, extracts the
   gzip-compressed tar archive there using libarchive, and searches the
   extracted top-level files for one with an MBR signature. The compressed
   input is removed after a matching image is found.
4. Creates mount and backup directories under `/opt/ota/extblock`, removes the
   previous boot backup, and copies `/boot` to
   `/opt/ota/extblock/old_boot_bkup`.
5. Obtains a loop device for the image and waits for the configured 200 ms
   stabilization period.
6. Marks the update as being in its critical phase. It mounts image partition
   2 read-only as the new root filesystem, chooses the passive root filesystem
   bank based on `/proc/cmdline`, mounts that bank, clears its existing
   contents, and copies the image root filesystem to it.
7. Mounts image partition 1 read-only as VFAT, clears `/boot`, and copies the
   image boot contents there. It then replaces the active bank path in
   `/boot/cmdline.txt` with the passive bank path.
8. Leaves the critical phase and processes any deferred termination signal.

Archive extraction must reach normal EOF and successfully write and finalize
every entry. Reader/writer initialization, header, data, finalization, or close
failures stop the update before loop setup or partition writes. The failure
is logged with the libarchive status and diagnostic when available, and
reader/writer resources are released on every exit path. Non-success
libarchive statuses, including warnings, are treated as extraction failures.

The configured banks are `/dev/mmcblk0p2` and `/dev/mmcblk0p3`. If
`/proc/cmdline` contains bank A, bank B is selected as passive; otherwise bank
A is selected.

## Resource Controls

The processor applies resource settings directly to itself before extraction
and copying:

| Setting | Configuration | Effect intended by the code |
| --- | --- | --- |
| CPU nice value | `CPU_NICE_LOW_PRIORITY = 10` | Lowers the flasher's CPU scheduling priority. |
| I/O scheduling class | `IO_PRIORITY_CLASS_IDLE = 3` | Requests idle I/O priority for the flasher. |
| CPU affinity | `CPU_AFFINITY_CORE_2 = 2`, `CPU_AFFINITY_CORE_3 = 3` | Restricts the flasher to CPUs 2 and 3. |

These are native process/system calls, not shell wrappers. They do not change
the caller's scheduling settings. The implementation does not check the
return values of the priority or affinity calls, so unsupported settings,
permissions, or unavailable CPUs do not by themselves abort the update.

## Configuration and Temporary Paths

`FlashConfig.hpp` centralizes the paths and constants used by the processor:

| Purpose | Configured value |
| --- | --- |
| Temporary OTA area | `/opt/ota` |
| Extracted image directory | `/opt/ota/wicimage` |
| Mount and backup root | `/opt/ota/extblock` |
| Boot mount and backup | `/opt/ota/extblock/ota_boot`, `/opt/ota/extblock/old_boot_bkup` |
| Root filesystem mounts | `/opt/ota/extblock/ota_rootfs`, `/opt/ota/extblock/target_rootfs` |
| Boot files | `/boot`, `/boot/cmdline.txt` |
| Loop device control | `/dev/loop-control`, with devices named `/dev/loop<N>` |
| Lock file | `/var/run/flash_app.lock` |
| Filesystems | `ext4` for rootfs, `vfat` for boot |

The paths and CPU numbers are compile-time constants; this implementation
does not read them from a runtime configuration file.

## Signals and Cleanup

Outside the critical phase, a received signal invokes cleanup and exits. During
the critical phase, the signal number is saved and termination is deferred
until the phase ends; the saved signal is then processed after cleanup.
Cleanup attempts to unmount all three mount paths, detach and close the loop
device, release and remove the instance lock, remove the temporary OTA
directories, and call `sync()`.

## Implementation Notes

- The root filesystem copy result is not used to stop the later boot-copy and
  command-line update sequence. The boot partition mount failure also does not
  immediately return from `flash()`.
- Failures during the update can leave partially written target partitions;
  cleanup releases resources but does not restore the passive root filesystem.
  The saved boot backup is created before the update, but automatic restoration
  is not performed by the cleanup routine.
- The code uses C++17 filesystem operations, Linux loop-device, mount,
  scheduling, and I/O-priority interfaces, and libarchive for gzip/tar
  extraction. Availability of BusyBox utilities alone is not sufficient for
  this native implementation.
