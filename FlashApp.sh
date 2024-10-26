#!/bin/sh

# Check and if exists, source these files /etc/include.properties and /etc/device.properties
if [ -f /etc/include.properties ]; then
    . /etc/include.properties
fi
if [ -f /etc/device.properties ]; then
    . /etc/device.properties
fi

if [ -z "$PERSISTENT_PATH" ]; then
    PERSISTENT_PATH="/opt"
fi

if [ -z "$LOG_PATH" ]; then
    LOG_PATH="/opt/logs"
    mkdir -p $LOG_PATH
fi

if [ -z "$FLASHAPPLOGFILE" ]; then
    FLASHAPPLOGFILE="$LOG_PATH/flashapp.log"
fi

EXTBLOCK="$PERSISTENT_PATH/ota/extblock"
WICIMAGEFILE="$PERSISTENT_PATH/ota/wicimage"

logger() {
    echo "$(date) FlashApp.sh > $1" | tee -a $FLASHAPPLOGFILE
}

# Check if all the commands used in this script are available
commandsRequired="date echo exit ls grep mkdir tar cp rm sync stat losetup mount umount reboot df sed awk md5sum trap mountpoint"
for cmd in $commandsRequired; do
    if ! command -v $cmd > /dev/null; then
        logger "Required command '$cmd' not found; cannot proceed, exiting."
        exit 1
    fi
done

# This script can be invoked with 2 arguments or 1 argument. Handle both cases.
# 1. /usr/bin/FlashApp "$DOWNLOAD_LOCATION/$UPGRADE_FILE"
# 2. /usr/bin/FlashApp "$DOWNLOAD_LOCATION" "$UPGRADE_FILE"
if [ $# -eq 2 ]; then
    cloudFWFile="$1/$2"
    logger "Invoked with two arguments '$1' and '$2'"
elif [ $# -eq 1 ]; then
    cloudFWFile="$1"
    logger "Invoked with single argument '$1'"
else
    logger "Invalid number of arguments passed"
    echo "Usage: $0 <Absolute path to Firmware Image File>"
    exit 1
fi

md5sumFile=$(md5sum $cloudFWFile | cut -d' ' -f1)
logger "Firmware image file received: '$cloudFWFile' with md5sum '$md5sumFile'"

# RPI OTA image is a compressed file; extract it if so.
if [ $(ls $cloudFWFile | grep -c "tar.gz") -eq 1 ]; then
    compressedFile=$cloudFWFile
    if [ -d $WICIMAGEFILE ]; then
        logger "Cleaning up the old directory '$WICIMAGEFILE'"
        rm -rf $WICIMAGEFILE && sync
    fi
    logger "Extracting the compressed firmware image file into '$WICIMAGEFILE'"
    mkdir -p $WICIMAGEFILE
    tar -xzf $cloudFWFile -C $WICIMAGEFILE && sync
    if [ $? -ne 0 ]; then
        logger "Failed to extract the compressed firmware image file; cannot proceed, exiting."
        rm -rf $WICIMAGEFILE $cloudFWFile && sync
        exit 1
    fi
    cloudFWFile=$(ls $WICIMAGEFILE/*.wic)
    if [ -z "$cloudFWFile" ]; then
        logger "Extracted firmware image file not found; cannot proceed, exiting."
        rm -rf $WICIMAGEFILE $cloudFWFile && sync
        exit 1
    fi
    # Remove the compressed file for space saving.
    logger "Removing the compressed firmware image file '$compressedFile'"
    rm -rf $compressedFile && sync
fi

# Check if the cloudFWFile exists and then print its size
if [ -f $cloudFWFile ]; then
    fileSize=$(stat -c %s $cloudFWFile)
    logger "File '$cloudFWFile' exists and its size is '$fileSize' bytes."
else
    logger "File '$cloudFWFile' does not exist; cannot proceed, exiting."
    exit 1
fi

logger "Creating the directory '$EXTBLOCK'"
mkdir -p $EXTBLOCK

# Check if the /boot partition exists; if not, exit.
boot_partition=$(df | grep "/boot" | cut -d' ' -f1)
ota_boot_mount_point=$EXTBLOCK/ota_boot
ota_rootfs_mount_point=$EXTBLOCK/ota_rootfs
target_rootfs_mount_point=$EXTBLOCK/target_rootfs
old_boot_bkup=$EXTBLOCK/old_boot_bkup

# Function to clean up the mounts and loop devices on exit condition including trap deregistration.
clean_up() {
    logger "Cleaning up the mounts and loop devices."
    for mountPointName in $ota_boot_mount_point $ota_rootfs_mount_point $target_rootfs_mount_point; do
        if [ "$mountPointName" == "$target_rootfs_mount_point" ] && [ $dontUmountPassiveBank -eq 1 ]; then
            # skip the passive bank mount point as its not mounted by this script.
            continue
        fi
        if mountpoint -q "$mountPointName"; then
            logger "Unmounting the mount point '$mountPointName'"
            umount $mountPointName
        fi
    done
    logger "Unmounting the loop device '$losetupNode'"
    losetup -d $losetupNode && sync
    if [ $? -ne 0 ]; then
        logger "Failed to unmount the loop device '$losetupNode'."
    fi
    rm -rf $EXTBLOCK
    rm -rf $WICIMAGEFILE
    if [ -f $cloudFWFile ]; then
        rm -f $cloudFWFile
    fi
    sync
    # deregister the signal handlers
    trap - INT TERM HUP
}

if [ -z "$boot_partition" ]; then
    logger "No /boot partition found; cannot proceed, exiting."
    exit 1
fi

# check if $old_boot_bkup exists, delete it and create a new one
if [ -d $old_boot_bkup ]; then
    logger "Cleaning up the previous /boot partition back-up '$old_boot_bkup'"
    rm -rf $old_boot_bkup && sync
fi

mkdir -p $EXTBLOCK/{ota_boot,ota_rootfs,target_rootfs,old_boot_bkup}
if [ ! -d $ota_boot_mount_point ] || [ ! -d $old_boot_bkup ] || [ ! -d $ota_rootfs_mount_point ] || [ ! -d $target_rootfs_mount_point ]; then
    logger "Failed to create '$ota_boot_mount_point', '$ota_rootfs_mount_point', '$target_rootfs_mount_point' or '$old_boot_bkup'; cannot proceed."
    exit 1
fi

logger "boot_partition: $boot_partition"
logger "ota_boot_mount_point: $ota_boot_mount_point"
logger "old_boot_bkup: $old_boot_bkup"
logger "ota_rootfs_mount_point: $ota_rootfs_mount_point"
logger "target_rootfs_mount_point: $target_rootfs_mount_point"

# TODO: block the signals to avoid any interruptions during the firmware update.

# back-up the contents of /boot partition
logger "Backing up the contents of '/boot' partition to '$old_boot_bkup'"
rm -rf $old_boot_bkup/* && sync
cp -ar /boot/* $old_boot_bkup/ && sync
if [ $? -ne 0 ]; then
    logger "Failed to back-up the contents of '$ota_boot_mount_point' partition; cannot proceed, exiting."
    clean_up
    exit 1
else
    logger "The '/boot/' back-up to '$old_boot_bkup' is successful."
fi

signal_handler() {
    logger "Update in progress, dismissing received signal '$1'."
}

# Prevent interrupts while updating the firmware.
trap 'signal_handler INT' INT
trap 'signal_handler TERM' TERM
trap 'signal_handler HUP' HUP

isRootFSUpdateSuccess=0
# Mount the WIC file using losetup and get the node name
losetupNode=$(losetup --find --show --partscan $cloudFWFile)
if [ -z "$losetupNode" ]; then
    logger "Failed to setup loop device for $cloudFWFile; cannot proceed, exiting."
    clean_up
    exit 1
fi
logger "Loop device '$losetupNode' is setup for $cloudFWFile"

# mount the P2 partition which is RootFS as readonly to avoid any accidental writes
ota_rootfs_node=$losetupNode"p2"
logger "Mounting the rootfs partition '$ota_rootfs_node' at '$ota_rootfs_mount_point' as read-only"

mount -o ro $ota_rootfs_node $ota_rootfs_mount_point
if [ $? -ne 0 ]; then
    echo "Failed to mount $cloudFWFile at $ota_rootfs_mount_point with offset $ota_rootfs_offset"
    clean_up
    exit 1
else
    # We have two rootfs partitions; one is the current rootfs and the other is the new rootfs.
    # identify the current rootfs partition and the new rootfs partition.
    activeBankDev=$(sed -e "s/.*root=//g" /proc/cmdline | cut -d ' ' -f1)
    if [ "$activeBankDev" == "/dev/mmcblk0p2" ]; then
        passiveBankDev="/dev/mmcblk0p3"
    else
        passiveBankDev="/dev/mmcblk0p2"
    fi
    logger "Active rootfs partition: '$activeBankDev' and Passive partition: '$passiveBankDev'."
    logger "Updating '$passiveBankDev' with the contents of '$ota_rootfs_mount_point'"

    dontUmountPassiveBank=0
    # check if the passive rootfs partition is already mounted; if so, use it.
    # TODO: handle RO mounted partitions.
    passiveBankMountPoint=$(mount | grep $passiveBankDev | awk '{print $3}')
    if [ -n "$passiveBankMountPoint" ]; then
        dontUmountPassiveBank=1
        logger "Passive rootfs partition '$passiveBankDev' is already mounted at '$passiveBankMountPoint'; proceeding with that."
        target_rootfs_mount_point=$passiveBankMountPoint
        logger "Using premounted target_rootfs_mount_point: $target_rootfs_mount_point"
    else
        mount -t ext4 $passiveBankDev $target_rootfs_mount_point
        if [ $? -ne 0 ]; then
            logger "Failed to mount the passive rootfs partition '$passiveBankDev'; cannot proceed, exiting."
            clean_up
            exit 1
        fi
    fi
    logger "Copying the contents of '$ota_rootfs_mount_point' to '$target_rootfs_mount_point'"
    rm -rf $target_rootfs_mount_point/* && sync
    cp -ar $ota_rootfs_mount_point/* $target_rootfs_mount_point/ && sync
    if [ $? -ne 0 ]; then
        logger "Failed to copy the contents of '$ota_rootfs_mount_point' to '$target_rootfs_mount_point'; revert and abort."
    else
        logger "The contents of '$ota_rootfs_mount_point' are copied to '$target_rootfs_mount_point' successfully."
        echo "OTA_UPDATED_ROOTFS="$(date)"" >> $target_rootfs_mount_point/version.txt
        isRootFSUpdateSuccess=1
    fi
    if [ $dontUmountPassiveBank -eq 0 ]; then
        umount $target_rootfs_mount_point
    fi
    umount $ota_rootfs_mount_point
fi

isBootUpdateSuccess=0

# mount the P1 partition which is Boot partition as readonly to avoid any accidental writes
ota_boot_node=$losetupNode"p1"
logger "Mounting the rootfs partition '$ota_boot_node' at '$ota_boot_mount_point' as read-only"
mount -o ro $ota_boot_node $ota_boot_mount_point
if [ $? -ne 0 ]; then
    logger "Failed to mount $ota_boot_node at $ota_boot_mount_point; exiting."
    clean_up
else
    logger "Copying the contents of '$ota_boot_mount_point' to '/boot' after cleaning it."
    rm -rf /boot/* && sync
    cp -ar $ota_boot_mount_point/* /boot/ && sync
    if [ $? -ne 0 ]; then
        logger "Failed to copy the contents of '$ota_boot_mount_point' to '/boot'; revert to old and abort."
        rm -rf /boot/* && sync && cp -ar $old_boot_bkup/* /boot/ && sync
        if [ $? -ne 0 ]; then
            logger "Failed to revert to old /boot partition; cannot proceed, exiting."
        else
            logger "Reverted '/boot' with contents of '$ota_boot_mount_point' successfully."
        fi
        clean_up
        exit 1
    else
        logger "The contents of '$ota_boot_mount_point' are copied to '/boot' successfully."
        isBootUpdateSuccess=1
    fi
    # may fail; ignore.
    umount $ota_boot_mount_point
fi

if [ $isBootUpdateSuccess -eq 1 ] && [ $isRootFSUpdateSuccess -eq 1 ]; then
    # Both boot and rootfs partitions are updated successfully; update the cmdline.txt to use new RootFS partition.
    logger "Updating the cmdline.txt to use the new rootfs partition '$passiveBankDev'"
    sed -i "s|$activeBankDev|$passiveBankDev|g" /boot/cmdline.txt && sync
    if [ $? -ne 0 ]; then
        logger "Failed to update the cmdline.txt; manual recovery required, exiting."
        clean_up
        exit 1
    else
        logger "The cmdline.txt is updated successfully to use '$passiveBankDev'."
        clean_up
    fi
else
    logger "The firmware update failed; cannot proceed, exiting."
    clean_up
    exit 1
fi

exit 0