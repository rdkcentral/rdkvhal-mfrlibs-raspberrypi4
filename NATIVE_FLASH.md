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

The entry point accepts an OTA path directly, or a directory and file name
with an optional notification interval:

```text
FlashApp <absolute path to OTA file>
FlashApp <directory path> <OTA file name>
FlashApp <directory path> <OTA file name> <notification interval in seconds>
```

Missing arguments print the usage forms and return status `1`. The
application installs handlers for `SIGINT`, `SIGTERM`, and `SIGHUP`, creates a
`NativeFlashProcessor` for the supplied path, and returns `0` when
`flash()` returns `mfrERR_NONE` or `1` otherwise. A handled signal exits with
that signal number. FlashApp logs each upgrade status callback using its phase
name and numeric value. The default notification interval is
`STATUS_NOTIFY_INTERVAL_SECONDS` (2 seconds). The optional interval must be a
non-negative integer; invalid or extra arguments are rejected.

## Status Reporting

`flash()` takes a `mfrUpgradeStatusNotify_t` and returns `mfrError_t`.
The blocking CLI uses the HAL status types with richer phase notifications:

- Errors found before flashing starts are returned without calling
  `notify.cb`: an active instance lock returns
  `mfrERR_OPERATION_NOT_SUPPORTED` and a missing image returns
  `mfrERR_IMAGE_FILE_OPEN_FAILED`.
- With a positive interval, phase transitions are reported immediately:
  `mfrUPGRADE_PROGRESS_NOT_STARTED`, `mfrUPGRADE_PROGRESS_STARTED`,
  `mfrUPGRADE_PROGRESS_VERIFYING`, and `mfrUPGRADE_PROGRESS_FLASHING`.
  Periodic callbacks repeat the current phase and percentage every
  `notify.interval` seconds. Even short phases are reported.
- An interval of `0` delivers only the final result, with no intermediate
  phase or periodic callbacks.
- Exactly one final callback is delivered: `mfrUPGRADE_PROGRESS_COMPLETED`
  with 100%, or `mfrUPGRADE_PROGRESS_ABORTED` with the error code and a short
  `error_string`.
- `flash()` blocks until the update finishes. `notify.cbData` is not passed to
  the callback because the HAL callback signature has no data argument.

The HAL entry point is different: `mfrWriteImage()` uses the C-callable bridge
in `NativeFlashBridge.cpp` and returns after synchronous input checks, lock
reservation, and worker creation. An error return never issues callbacks.
After acceptance, the worker owns the processor, image path, and a copy of the
notification configuration. Nonterminal HAL callbacks report only `STARTED`;
the CLI's `NOT_STARTED`, `VERIFYING`, and `FLASHING` phases are not forwarded.
Post-start failures produce `ABORTED` without changing the successful API return.
An interval of zero still delivers only the final result. The HAL worker installs
no process signal handlers. `mfr_term()` waits for work and callbacks to finish;
it cannot be called from a flash callback. Caller callback resources must remain
valid until completion, and the library must not be unloaded before termination.

`ABORTED` is an alternative to `COMPLETED`, not a normal success phase.
`VERIFYING` covers initial input/archive checks, partition-table validation,
active bank selection, passive-bank unmounting and capacity checks, and the
`/boot` backup. `FLASHING` starts at 40%, immediately before the first partition
write, only after those checks succeed. An oversized WIC rootfs therefore aborts
from `VERIFYING` without a `FLASHING` notification. The flasher validates MBR
geometry and GPT checksums when applicable; it does not verify a cryptographic
image signature. Gzip/tar data and trailer integrity checks continue during
streaming, so late corruption can still abort from `FLASHING`. That phase
continues through boot copying and the bank switch.

Progress is measured within weighted stages, rather than advanced by a timer:

| Work | Percentage range | Measurement |
| --- | --- | --- |
| Boot backup and preparation | 0-40% | Bytes copied |
| Raw partition streaming and validation | 40-80% | Boot and rootfs partition bytes written |
| Boot partition copy | 80-90% | Bytes copied |
| Boot command-line update | 90-99% | Successful bank-switch update |

Raw partition progress counts bytes written to a staged boot image and the
passive rootfs. Rootfs data is flushed periodically and synchronized before
the bank switch; progress is not advanced by a timer. Percentages never
decrease, and 100% is reserved for successful completion.

| Failure | Returned error |
| --- | --- |
| Boot loop-device setup | `mfrERR_SRC_FILE_ERROR` |
| Invalid partition table or staged boot image mount failure | `mfrERR_BAD_IMAGE_HEADER` |
| Input/archive read failure, passive-bank write, boot copy, or filesystem exception | `mfrERR_WRITE_FLASH_FAILED` |
| `/boot/cmdline.txt` update | `mfrERR_UPDATE_BOOT_PARAMS_FAILED` |

## Update Workflow

`NativeFlashProcessor::flash()` performs the following operations:

1. Acquires an exclusive, non-blocking lock at `/var/run/flash_app.lock`. A
   second instance reports that an update is active and aborts.
2. Checks that the input path exists and applies the configured CPU priority,
   I/O priority, and CPU affinity settings.
3. Reads exactly one `root=` argument from `/proc/cmdline`; only
   `/dev/mmcblk0p2` and `/dev/mmcblk0p3` are accepted. The other configured
   bank is selected as the passive target. Missing, duplicate, or unsupported
   root arguments fail closed.
4. Unmounts every mount point of the passive bank (for example `/root2`, which
   is mounted read-write at boot). Mounts are removed newest first with a normal
   (non-lazy) unmount, and the pass repeats until the device is absent from
   `/proc/self/mountinfo`. The running root filesystem is never unmounted. If
   any mount point is busy, the update fails before anything is written. The
   passive bank is not remounted; it is mounted again at the next boot.
5. Verifies that `/boot` is a writable vfat mount of `/dev/mmcblk0p1`, then backs
   it up to `/opt/ota/extblock/old_boot_bkup` before writing either partition.
   Missing, wrong-device, or read-only mounts fail closed. The mount is checked
   again before streaming, boot replacement, rollback, and the bank switch.
   An existing backup is never overwritten: resolve it manually before retrying.
6. Streams a raw `.wic` file or the single regular `.wic` member of a
   `.wic.tar.gz` archive. It does not extract the full disk image. The primary
   MBR or GPT partition table is bounds-checked; GPT header and entry-array
   CRCs are verified. Extended and hybrid MBR layouts are rejected.
7. Stages WIC partition 1 in a uniquely created file under `/run`, and writes
   WIC partition 2 directly to the passive rootfs block device. The device is
   opened exclusively without following symlinks, verified to be a block
   device, checked against `/proc/self/mountinfo`, and sized with
   `BLKGETSIZE64`. Any error in the size check or a partition that does not fit
   aborts before the first block write. Target writes are checked for short,
   zero-length, and failed writes, periodically flushed, and synchronized
   before continuing.
8. Drains the WIC entry and the complete archive to normal EOF, checking all
   libarchive statuses so gzip/tar trailer errors abort the update. The
   passive rootfs may already have been overwritten if corruption is detected
   late, but `/boot/cmdline.txt` is not switched, so the active bank remains
   selected. The OTA input is retained on failure and removed only after a
   successful bank switch when it is gzip-compressed.
9. Mounts the staged boot image read-only through a loop device, updates `/boot`
   from it, then writes a temporary `cmdline.txt`, synchronizes it, and renames
   it over the original. A boot update or bank-switch failure attempts to
   restore `/boot` from the backup. Both replacement and rollback preserve
   `/boot/first_use_date.txt` in place, including its FAT creation time; an image
   or backup entry with that name is not copied over the device-specific marker.
   A failed restoration retains the backup and reports its location for recovery.
10. Leaves the critical phase, reports the final status, and processes any
   deferred termination signal.

The WIC layout must contain primary partitions 1 (boot) and 2 (rootfs), and
the WIC rootfs partition must fit in the passive device. The flasher does not
run `resize2fs`; it writes the filesystem image as built. A `--size` value for
a `--source rootfs` WIC partition is a minimum and may be increased by WIC's
overhead factor. Use a fixed-size build setting if both banks must be exactly
1024 MiB, and keep the runtime capacity check. Writing a filesystem image to
the other bank duplicates its ext4 UUID and label; root selection must use the
device path shown above rather than an ambiguous filesystem UUID or label.

## Resource Controls

The processor applies resource settings directly to itself before streaming
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
| Mount and backup root | `/opt/ota/extblock` |
| Boot mount and backup | `/opt/ota/extblock/ota_boot`, `/opt/ota/extblock/old_boot_bkup` |
| Staged WIC boot partition | Unique file under `/run` |
| Boot files | `/boot`, `/boot/cmdline.txt` |
| Loop device control | `/dev/loop-control`, with devices named `/dev/loop<N>` |
| Lock file | `/var/run/flash_app.lock` |
| Mounted filesystem | `vfat` for the staged boot partition |

The paths and CPU numbers are compile-time constants; this implementation
does not read them from a runtime configuration file.

## Signals and Cleanup

The signal handler only assigns a `volatile sig_atomic_t` flag. Normal execution
checks the flag during preparation and backup, reports an abort, and cleans up.
During the critical phase, termination is deferred until the update or rollback
finishes; logging, cleanup, and the signal-number exit status happen outside the
handler.
Cleanup unmounts the staged boot partition, detaches and closes the loop
device, removes the staged boot image after detaching it, releases the instance
lock, and removes the temporary mount directory. The boot backup is deleted
only after a successful update or confirmed restoration; otherwise it and its
parent directory are retained. The lock file
itself remains in place so concurrent invocations always lock the same inode.

## Implementation Notes

- Filesystem errors during boot copying are caught and reported as
  `mfrERR_WRITE_FLASH_FAILED`; the bank switch only happens after the raw
  rootfs write and boot update succeed.
- Failures during the update can leave a partially written passive root
  filesystem; cleanup releases resources but does not restore it. The active
  bank stays selected in that case.
- Raw copying duplicates the ext4 UUID and label from the WIC root partition.
  Avoid mounting either bank by that duplicated filesystem UUID or label.
- The code uses C++17 filesystem operations, Linux block-device and
  loop-device interfaces, mount, scheduling, and I/O-priority interfaces, and
  libarchive for gzip/tar streaming. Availability of BusyBox utilities alone
  is not sufficient for this native implementation.
