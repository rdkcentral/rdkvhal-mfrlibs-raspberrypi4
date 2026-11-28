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

#include "WicStreamFlasher.hpp"
#include "FlashConfig.hpp"

#include <algorithm>
#include <array>
#include <archive.h>
#include <archive_entry.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/sysmacros.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <linux/fs.h>

namespace {

// ---- Sizes and streaming limits ----
constexpr std::uint64_t kSectorSize = 512;
constexpr std::size_t kMebibyte = 1024 * 1024;
// Bytes buffered from the image start; covers the MBR, the GPT header and its entry array.
constexpr std::size_t kPartitionTableBufferSize = kMebibyte;
constexpr std::size_t kChunkSize = kMebibyte;
// Rootfs writes are flushed with fdatasync() after this many bytes to bound dirty page cache.
constexpr std::uint64_t kFlushInterval = 64ULL * kMebibyte;
constexpr std::size_t kMaxArchiveEntries = 4096;
constexpr std::uint64_t kMaxAuxiliaryArchiveBytes = 64ULL * kMebibyte;

// ---- WIC layout: ----
// Refer: https://github.com/rdkcentral/meta-vendor-raspberrypi-release/blob/develop/wic/sdimage-raspberrypi.wks
constexpr int kWicBootPartitionNumber = 1;
constexpr int kWicRootfsPartitionNumber = 2;
constexpr char kWicExtension[] = ".wic";
constexpr char kBootImageTemplate[] = "/run/flashapp-boot-XXXXXX";

// ---- MBR (LBA 0) layout ----
constexpr std::size_t kMbrPartitionTableOffset = 446;
constexpr std::size_t kMbrPartitionEntrySize = 16;
constexpr std::size_t kMbrPartitionEntryCount = 4;
constexpr std::size_t kMbrEntryStatusOffset = 0;
constexpr std::size_t kMbrEntryTypeOffset = 4;
constexpr std::size_t kMbrEntryStartLbaOffset = 8;
constexpr std::size_t kMbrEntrySectorCountOffset = 12;
constexpr std::size_t kMbrBootSignatureOffset = 510;
constexpr std::uint8_t kMbrBootSignatureByte0 = 0x55;
constexpr std::uint8_t kMbrBootSignatureByte1 = 0xAA;
constexpr std::uint8_t kMbrStatusInactive = 0x00;
constexpr std::uint8_t kMbrStatusBootable = 0x80;
constexpr std::uint8_t kMbrTypeEmpty = 0x00;
constexpr std::uint8_t kMbrTypeExtendedChs = 0x05;
constexpr std::uint8_t kMbrTypeExtendedLba = 0x0F;
constexpr std::uint8_t kMbrTypeLinuxExtended = 0x85;
constexpr std::uint8_t kProtectiveMbrType = 0xEE;

// ---- GPT header (LBA 1) layout, UEFI specification section 5.3.2 ----
constexpr std::uint64_t kGptHeaderLba = 1;
// The GPT header must lie within the buffered data, and entries/partitions after it.
constexpr std::uint64_t kGptHeaderEndOffset = (kGptHeaderLba + 1) * kSectorSize;
constexpr char kGptSignature[] = "EFI PART";
constexpr std::size_t kGptSignatureSize = sizeof(kGptSignature) - 1;
constexpr std::size_t kGptHeaderSizeOffset = 12;
constexpr std::size_t kGptHeaderCrcOffset = 16;
constexpr std::size_t kGptCurrentLbaOffset = 24;
constexpr std::size_t kGptBackupLbaOffset = 32;
constexpr std::size_t kGptFirstUsableLbaOffset = 40;
constexpr std::size_t kGptLastUsableLbaOffset = 48;
constexpr std::size_t kGptEntriesLbaOffset = 72;
constexpr std::size_t kGptEntryCountOffset = 80;
constexpr std::size_t kGptEntrySizeOffset = 84;
constexpr std::size_t kGptEntriesCrcOffset = 88;
constexpr std::size_t kGptCrcFieldSize = sizeof(std::uint32_t);
constexpr std::uint32_t kGptMinimumHeaderSize = 92;
// LBA 0 holds the protective MBR and LBA 1 the GPT header.
constexpr std::uint64_t kGptMinimumFirstUsableLba = 2;
// Sanity limits; the specification requires at least 128 entries of 128 bytes each.
constexpr std::uint32_t kGptMaximumEntryCount = 16384;
constexpr std::uint32_t kGptMinimumEntrySize = 128;
constexpr std::uint32_t kGptMaximumEntrySize = 4096;
constexpr std::uint32_t kGptEntrySizeAlignment = 8;

// ---- GPT partition entry layout ----
constexpr std::size_t kGptEntryTypeGuidSize = 16;
constexpr std::size_t kGptEntryFirstLbaOffset = 32;
constexpr std::size_t kGptEntryLastLbaOffset = 40;

// ---- CRC-32 (IEEE 802.3, reflected) as used by GPT ----
constexpr std::uint32_t kCrc32InitialValue = 0xFFFFFFFFU;
constexpr std::uint32_t kCrc32ReflectedPolynomial = 0xEDB88320U;
constexpr int kBitsPerByte = 8;

// ---- Input format detection ----
constexpr std::array<std::uint8_t, 2> kGzipMagic {0x1F, 0x8B};

// ---- Mount table ----
constexpr char kMountInfoPath[] = "/proc/self/mountinfo";
// mountinfo escapes special characters in paths as a backslash followed by three octal digits.
constexpr std::size_t kMountInfoEscapeDigits = 3;
constexpr int kOctalBase = 8;
// Unmounting the top mount can expose an earlier mount stacked on the same path.
constexpr int kMaximumUnmountPasses = 8;

class UniqueFd {
public:
    explicit UniqueFd(int descriptor = -1) noexcept : m_descriptor(descriptor) {}
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    int get() const noexcept { return m_descriptor; }

    int release() noexcept
    {
        const int descriptor = m_descriptor;
        m_descriptor = -1;
        return descriptor;
    }

    void reset(int descriptor = -1) noexcept
    {
        if (m_descriptor >= 0) {
            ::close(m_descriptor);
        }
        m_descriptor = descriptor;
    }

private:
    int m_descriptor;
};

struct PartitionRange {
    int index;
    std::uint64_t start_byte;
    std::uint64_t byte_length;
};

struct TargetFile {
    UniqueFd descriptor;
    std::string path;
    bool block_device = false;
    bool remove_on_destroy = false;

    ~TargetFile()
    {
        if (remove_on_destroy && !path.empty()) {
            ::unlink(path.c_str());
        }
    }

    TargetFile() = default;
    TargetFile(const TargetFile&) = delete;
    TargetFile& operator=(const TargetFile&) = delete;
};

struct StreamState {
    std::uint64_t bytes_written = 0;
    std::uint64_t total_bytes = 0;
};

WicStreamResult failure(int error_code, std::string message)
{
    return {false, error_code, std::move(message), {}, 0, 0};
}

template <typename Integer>
Integer read_little_endian(const std::uint8_t* data)
{
    Integer value = 0;
    for (std::size_t byte_index = 0; byte_index < sizeof(Integer); ++byte_index) {
        value |= static_cast<Integer>(data[byte_index]) << (byte_index * kBitsPerByte);
    }
    return value;
}

std::uint32_t read_le32(const std::uint8_t* data)
{
    return read_little_endian<std::uint32_t>(data);
}

std::uint64_t read_le64(const std::uint8_t* data)
{
    return read_little_endian<std::uint64_t>(data);
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size)
{
    std::uint32_t crc = kCrc32InitialValue;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < kBitsPerByte; ++bit) {
            crc = (crc >> 1) ^ (kCrc32ReflectedPolynomial & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

bool checked_multiply(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& result)
{
    if (rhs != 0 && lhs > std::numeric_limits<std::uint64_t>::max() / rhs) {
        return false;
    }
    result = lhs * rhs;
    return true;
}

bool checked_add(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& result)
{
    if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

/**
 * @brief Check that a set of partition ranges do not overlap and are valid.
 * @param ranges The partition ranges to check.
 * @param error Output string to receive an error message if validation fails.
 * @return True if the ranges are valid and non-overlapping, false otherwise.
 */
bool ranges_do_not_overlap(std::vector<PartitionRange>& ranges, std::string& error)
{
    std::sort(ranges.begin(), ranges.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.start_byte < rhs.start_byte;
    });
    // No partition may overlap the MBR sector.
    std::uint64_t previous_end = kSectorSize;
    for (const auto& range : ranges) {
        std::uint64_t range_end = 0;
        if (range.byte_length == 0 || !checked_add(range.start_byte, range.byte_length, range_end) ||
            range.start_byte < previous_end) {
            error = "Partition table contains an empty, overflowing, or overlapping partition";
            return false;
        }
        previous_end = range_end;
    }
    return true;
}

/**
 * @brief Parse the MBR and GPT partition tables from a WIC image header.
 * @param header The first 1 MiB of the WIC image, containing the MBR and GPT header/entries.
 * @param image_size The total size of the WIC image in bytes.
 * @param ranges Output vector to receive the parsed partition ranges.
 * @param error Output string to receive an error message if parsing fails.
 * @return True if parsing succeeds, false otherwise.
 */
bool parse_mbr_partitions(const std::vector<std::uint8_t>& header,
                          std::uint64_t image_size,
                          std::vector<PartitionRange>& ranges,
                          std::string& error)
{
    if (header.size() < kSectorSize ||
        header[kMbrBootSignatureOffset] != kMbrBootSignatureByte0 ||
        header[kMbrBootSignatureOffset + 1] != kMbrBootSignatureByte1) {
        error = "WIC image has no valid MBR signature";
        return false;
    }
    if (image_size % kSectorSize != 0) {
        error = "WIC image size is not a whole number of sectors";
        return false;
    }

    const std::uint8_t* const mbr = header.data();
    const auto mbr_entry_offset = [](std::size_t entry_index) {
        return kMbrPartitionTableOffset + entry_index * kMbrPartitionEntrySize;
    };
    const bool protective_gpt = mbr[mbr_entry_offset(0) + kMbrEntryTypeOffset] == kProtectiveMbrType;
    // If the MBR is protective, the GPT header and entries must be present in the buffered data.
    if (protective_gpt) {
        for (std::size_t index = 1; index < kMbrPartitionEntryCount; ++index) {
            const std::size_t offset = mbr_entry_offset(index);
            if (mbr[offset + kMbrEntryTypeOffset] != kMbrTypeEmpty ||
                read_le32(mbr + offset + kMbrEntryStartLbaOffset) != 0 ||
                read_le32(mbr + offset + kMbrEntrySectorCountOffset) != 0) {
                error = "Hybrid GPT/MBR partition tables are not supported";
                return false;
            }
        }
        if (header.size() < kGptHeaderEndOffset ||
            std::memcmp(header.data() + kGptHeaderLba * kSectorSize, kGptSignature, kGptSignatureSize) != 0) {
            error = "Protective MBR does not contain a valid GPT header";
            return false;
        }

        const std::uint8_t* const gpt = header.data() + kGptHeaderLba * kSectorSize;
        const std::uint32_t header_size = read_le32(gpt + kGptHeaderSizeOffset);
        const std::uint64_t total_sectors = image_size / kSectorSize;
        const std::uint64_t current_lba = read_le64(gpt + kGptCurrentLbaOffset);
        const std::uint64_t backup_lba = read_le64(gpt + kGptBackupLbaOffset);
        const std::uint64_t first_usable_lba = read_le64(gpt + kGptFirstUsableLbaOffset);
        const std::uint64_t last_usable_lba = read_le64(gpt + kGptLastUsableLbaOffset);
        const std::uint64_t entries_lba = read_le64(gpt + kGptEntriesLbaOffset);
        const std::uint32_t entry_count = read_le32(gpt + kGptEntryCountOffset);
        const std::uint32_t entry_size = read_le32(gpt + kGptEntrySizeOffset);
        const std::uint32_t expected_header_crc = read_le32(gpt + kGptHeaderCrcOffset);
        const std::uint32_t expected_entries_crc = read_le32(gpt + kGptEntriesCrcOffset);

        if (header_size < kGptMinimumHeaderSize || header_size > kSectorSize ||
            current_lba != kGptHeaderLba ||
            backup_lba >= total_sectors || backup_lba == current_lba ||
            first_usable_lba < kGptMinimumFirstUsableLba || last_usable_lba < first_usable_lba ||
            last_usable_lba >= total_sectors || backup_lba <= last_usable_lba || entry_count == 0 ||
            entry_count > kGptMaximumEntryCount ||
            entry_size < kGptMinimumEntrySize || entry_size > kGptMaximumEntrySize ||
            (entry_size % kGptEntrySizeAlignment) != 0) {
            error = "GPT header contains invalid geometry";
            return false;
        }
        std::uint64_t entry_table_size = 0;
        std::uint64_t entry_table_offset = 0;
        std::uint64_t entry_table_end = 0;
        if (!checked_multiply(entry_count, entry_size, entry_table_size) ||
            !checked_multiply(entries_lba, kSectorSize, entry_table_offset) ||
            !checked_add(entry_table_offset, entry_table_size, entry_table_end) ||
            entry_table_offset < kGptHeaderEndOffset ||
            entry_table_end > header.size() ||
            entry_table_end > image_size ||
            entry_table_end > first_usable_lba * kSectorSize) {
            error = "GPT partition table exceeds validated image header bounds";
            return false;
        }

        // The header CRC is computed with its own CRC field zeroed.
        std::vector<std::uint8_t> header_copy(gpt, gpt + header_size);
        std::fill(header_copy.begin() + kGptHeaderCrcOffset,
                  header_copy.begin() + kGptHeaderCrcOffset + kGptCrcFieldSize, 0);
        if (crc32(header_copy.data(), header_copy.size()) != expected_header_crc ||
            crc32(header.data() + entry_table_offset, static_cast<std::size_t>(entry_table_size)) != expected_entries_crc) {
            error = "GPT header or partition-table checksum is invalid";
            return false;
        }

        for (std::uint32_t index = 0; index < entry_count; ++index) {
            const std::uint8_t* const entry =
                header.data() + entry_table_offset + static_cast<std::uint64_t>(index) * entry_size;
            // An all-zero partition type GUID marks an unused entry.
            const bool unused = std::all_of(entry, entry + kGptEntryTypeGuidSize,
                                            [](std::uint8_t byte) { return byte == 0; });
            if (unused) {
                continue;
            }
            const std::uint64_t first_lba = read_le64(entry + kGptEntryFirstLbaOffset);
            const std::uint64_t last_lba = read_le64(entry + kGptEntryLastLbaOffset);
            if (first_lba < first_usable_lba || last_lba < first_lba ||
                last_lba > last_usable_lba) {
                error = "GPT partition entry contains invalid LBA bounds";
                return false;
            }
            std::uint64_t start_byte = 0;
            std::uint64_t sector_count = 0;
            std::uint64_t byte_length = 0;
            // GPT LBA ranges are inclusive, and partition numbers are 1-based.
            if (!checked_multiply(first_lba, kSectorSize, start_byte) ||
                !checked_add(last_lba - first_lba, 1, sector_count) ||
                !checked_multiply(sector_count, kSectorSize, byte_length)) {
                error = "GPT partition geometry overflows";
                return false;
            }
            ranges.push_back({static_cast<int>(index + 1), start_byte, byte_length});
        }
    } else {
        // MBR partition table
        for (std::size_t index = 0; index < kMbrPartitionEntryCount; ++index) {
            const std::size_t offset = mbr_entry_offset(index);
            const std::uint8_t status = mbr[offset + kMbrEntryStatusOffset];
            const std::uint8_t type = mbr[offset + kMbrEntryTypeOffset];
            const std::uint32_t start_lba = read_le32(mbr + offset + kMbrEntryStartLbaOffset);
            const std::uint32_t sector_count = read_le32(mbr + offset + kMbrEntrySectorCountOffset);
            if (type == kMbrTypeEmpty && start_lba == 0 && sector_count == 0) {
                continue;
            }
            const bool extended_type = type == kMbrTypeExtendedChs || type == kMbrTypeExtendedLba ||
                                       type == kMbrTypeLinuxExtended;
            if ((status != kMbrStatusInactive && status != kMbrStatusBootable) ||
                type == kMbrTypeEmpty || type == kProtectiveMbrType || extended_type ||
                start_lba == 0 || sector_count == 0) {
                error = "MBR contains an invalid or unsupported partition entry";
                return false;
            }
            std::uint64_t start_byte = 0;
            std::uint64_t byte_length = 0;
            std::uint64_t end_byte = 0;
            if (!checked_multiply(start_lba, kSectorSize, start_byte) ||
                !checked_multiply(sector_count, kSectorSize, byte_length) ||
                !checked_add(start_byte, byte_length, end_byte) || end_byte > image_size) {
                error = "MBR partition geometry exceeds the WIC image";
                return false;
            }
            ranges.push_back({static_cast<int>(index + 1), start_byte, byte_length});
        }
    }

    if (ranges.empty() || !ranges_do_not_overlap(ranges, error)) {
        if (error.empty()) {
            error = "WIC partition table contains no usable partitions";
        }
        return false;
    }
    for (const auto& range : ranges) {
        std::uint64_t end_byte = 0;
        if (range.start_byte < kPartitionTableBufferSize ||
            !checked_add(range.start_byte, range.byte_length, end_byte) || end_byte > image_size) {
            error = "Partition extends beyond the WIC image";
            return false;
        }
    }
    return true;
}

/**
 * @brief Read an exact number of bytes from a reader function into a destination buffer.
 * @param reader A function that reads bytes into a buffer and returns the number of bytes read.
 * @param destination The buffer to read bytes into.
 * @param byte_count The exact number of bytes to read.
 * @param error Output string to receive an error message if reading fails.
 * @return True if the exact number of bytes was read successfully, false otherwise.
 */
bool read_exact(const std::function<la_ssize_t(std::uint8_t*, std::size_t, std::string&)>& reader,
                std::uint8_t* destination,
                std::size_t byte_count,
                std::string& error)
{
    std::size_t total_read = 0;
    while (total_read < byte_count) {
        const la_ssize_t result = reader(destination + total_read, byte_count - total_read, error);
        if (result < 0) {
            if (error.empty()) {
                error = "Input read failed";
            }
            return false;
        }
        if (result == 0) {
            error = "Unexpected end of image data";
            return false;
        }
        if (static_cast<std::uint64_t>(result) > byte_count - total_read) {
            error = "Input reader returned more data than requested";
            return false;
        }
        total_read += static_cast<std::size_t>(result);
    }
    return true;
}

/**
 * @brief Read and discard a specified number of bytes from a reader function.
 * @param reader A function that reads bytes into a buffer and returns the number of bytes read.
 * @param byte_count The number of bytes to read and discard.
 * @param scratch A buffer used for temporary storage while discarding bytes.
 * @param error Output string to receive an error message if reading fails.
 * @return True if the specified number of bytes was read and discarded successfully, false otherwise.
 */
bool read_and_discard(
    const std::function<la_ssize_t(std::uint8_t*, std::size_t, std::string&)>& reader,
    std::uint64_t byte_count,
    std::array<std::uint8_t, kChunkSize>& scratch,
    std::string& error)
{
    while (byte_count > 0) {
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(scratch.size(), byte_count));
        if (!read_exact(reader, scratch.data(), chunk, error)) {
            return false;
        }
        byte_count -= chunk;
    }
    return true;
}

/**
 * @brief Decode a mountinfo-encoded path, replacing octal escapes with their character equivalents.
 * Why: mountinfo escapes space, tab, newline and backslash in paths as three-digit octal (e.g. "\040").
 * @param encoded_path The mountinfo-encoded path string.
 * @return The decoded path string.
 */
std::string decode_mountinfo_path(const std::string& encoded_path)
{
    const auto is_octal = [](char character) { return character >= '0' && character <= '7'; };
    std::string decoded_path;
    decoded_path.reserve(encoded_path.size());
    for (std::size_t index = 0; index < encoded_path.size(); ++index) {
        const bool escape_fits = encoded_path[index] == '\\' &&
                                 index + kMountInfoEscapeDigits < encoded_path.size();
        bool is_escape = escape_fits;
        for (std::size_t digit = 1; is_escape && digit <= kMountInfoEscapeDigits; ++digit) {
            is_escape = is_octal(encoded_path[index + digit]);
        }
        if (is_escape) {
            int value = 0;
            for (std::size_t digit = 1; digit <= kMountInfoEscapeDigits; ++digit) {
                value = value * kOctalBase + (encoded_path[index + digit] - '0');
            }
            decoded_path.push_back(static_cast<char>(value));
            index += kMountInfoEscapeDigits;
        } else {
            decoded_path.push_back(encoded_path[index]);
        }
    }
    return decoded_path;
}

/**
 * @brief Collect every mount point of a block device, in mount order.
 * @param device_number The device number of the block device to check.
 * @param mount_points Output vector to receive the mount points of the device.
 * @return false if the mount table cannot be read completely.
 */
bool find_mount_points(dev_t device_number, std::vector<std::string>& mount_points)
{
    mount_points.clear();
    std::ifstream mount_info(kMountInfoPath);
    if (!mount_info) {
        return false;
    }
    const std::string expected = std::to_string(major(device_number)) + ":" +
                                 std::to_string(minor(device_number));
    std::string line;
    while (std::getline(mount_info, line)) {
        std::istringstream fields(line);
        std::string mount_id;
        std::string parent_id;
        std::string device_id;
        std::string root;
        std::string mount_point;
        if (!(fields >> mount_id >> parent_id >> device_id >> root >> mount_point)) {
            return false;
        }
        if (device_id == expected) {
            mount_points.push_back(decode_mountinfo_path(mount_point));
        }
    }
    return !mount_info.bad() && mount_info.eof();
}

/**
 * @brief Check if a block device is currently mounted.
 * @param device_number The device number of the block device to check.
 * @return true if the device is mounted or if the mount state cannot be determined, false otherwise.
 */
bool device_is_mounted(dev_t device_number)
{
    std::vector<std::string> mount_points;
    return !find_mount_points(device_number, mount_points) || !mount_points.empty();
}

/**
 * @brief Write all bytes from a buffer to a file descriptor at a specified offset.
 * @param descriptor The file descriptor to write to.
 * @param data The buffer containing the data to write.
 * @param byte_count The number of bytes to write from the buffer.
 * @param offset The offset in the file descriptor at which to start writing.
 * @param error Output string to receive an error message if writing fails.
 * @return True if all bytes were written successfully, false otherwise.
 */
bool write_all_at(int descriptor,
                  const std::uint8_t* data,
                  std::size_t byte_count,
                  std::uint64_t offset,
                  std::string& error)
{
    std::size_t total_written = 0;
    while (total_written < byte_count) {
        const std::uint64_t current_offset = offset + total_written;
        if (current_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
            error = "Partition write offset exceeds the supported file offset range";
            return false;
        }
        const ssize_t result = ::pwrite(descriptor, data + total_written, byte_count - total_written,
                                        static_cast<off_t>(current_offset));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = std::string("Partition write failed: ") + std::strerror(errno);
            return false;
        }
        if (result == 0) {
            error = "Partition write returned zero bytes";
            return false;
        }
        total_written += static_cast<std::size_t>(result);
    }
    return true;
}

/**
 * @brief Create temporary files for the boot partition and open the root filesystem device.
 * @param boot_partition The partition range for the boot partition.
 * @param required_rootfs_bytes The required size of the root filesystem in bytes.
 * @param rootfs_device The path to the root filesystem device.
 * @param boot_file Output TargetFile for the temporary boot partition file.
 * @param rootfs Output TargetFile for the root filesystem device.
 * @param error Output string to receive an error message if creation fails.
 * @param error_code Output integer to receive the error code if creation fails.
 * @return True if the target files were created/opened successfully, false otherwise.
 */
bool create_target_file(const PartitionRange& boot_partition,
                        std::uint64_t required_rootfs_bytes,
                        const std::string& rootfs_device,
                        TargetFile& boot_file,
                        TargetFile& rootfs,
                        std::string& error,
                        int& error_code)
{
    std::array<char, sizeof(kBootImageTemplate)> template_buffer {};
    std::memcpy(template_buffer.data(), kBootImageTemplate, sizeof(kBootImageTemplate));
    const int boot_descriptor = ::mkstemp(template_buffer.data());
    if (boot_descriptor < 0) {
        error_code = errno;
        error = std::string("Cannot create temporary boot image: ") + std::strerror(error_code);
        return false;
    }
    boot_file.descriptor.reset(boot_descriptor);
    boot_file.path = template_buffer.data();
    boot_file.remove_on_destroy = true;
    if (::fcntl(boot_descriptor, F_SETFD, FD_CLOEXEC) < 0) {
        error_code = errno;
        error = std::string("Cannot secure temporary boot image: ") + std::strerror(error_code);
        return false;
    }
    if (boot_partition.byte_length > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        error_code = EOVERFLOW;
        error = "Temporary boot image size exceeds supported file offsets";
        return false;
    }
    if (::ftruncate(boot_descriptor, static_cast<off_t>(boot_partition.byte_length)) != 0) {
        error_code = errno;
        error = std::string("Cannot size temporary boot image: ") + std::strerror(error_code);
        return false;
    }

    const int rootfs_descriptor = ::open(rootfs_device.c_str(),
                                          O_RDWR | O_EXCL | O_CLOEXEC | O_NOFOLLOW);
    if (rootfs_descriptor < 0) {
        error_code = errno;
        error = std::string("Cannot exclusively open passive root filesystem ") +
                rootfs_device + ": " + std::strerror(error_code);
        return false;
    }
    struct stat target_status {};
    if (::fstat(rootfs_descriptor, &target_status) != 0) {
        error_code = errno;
        error = std::string("Cannot inspect target device: ") + std::strerror(error_code);
        ::close(rootfs_descriptor);
        return false;
    }
    if (!S_ISBLK(target_status.st_mode)) {
        error_code = ENOTBLK;
        error = "Root filesystem target is not a block device";
        ::close(rootfs_descriptor);
        return false;
    }
    if (device_is_mounted(target_status.st_rdev)) {
        error_code = EBUSY;
        error = "Root filesystem target is mounted or mount state cannot be checked";
        ::close(rootfs_descriptor);
        return false;
    }

    std::uint64_t device_size = 0;
    if (::ioctl(rootfs_descriptor, BLKGETSIZE64, &device_size) != 0) {
        error_code = errno;
        error = std::string("Cannot determine target block-device size: ") + std::strerror(error_code);
        ::close(rootfs_descriptor);
        return false;
    }
    if (device_size < required_rootfs_bytes) {
        error_code = ENOSPC;
        error = "Passive root filesystem partition is smaller than the WIC root partition";
        ::close(rootfs_descriptor);
        return false;
    }
    rootfs.descriptor.reset(rootfs_descriptor);
    rootfs.path = rootfs_device;
    rootfs.block_device = true;
    return true;
}

/**
 * @brief Consume partition data from a WIC image and write it to the boot and rootfs targets.
 * @param reader A function that reads bytes from the WIC image into a buffer.
 * @param image_size The total size of the WIC image in bytes.
 * @param rootfs_device The path to the root filesystem device.
 * @param boot_file Output TargetFile for the temporary boot partition file.
 * @param rootfs_file Output TargetFile for the root filesystem device.
 * @param progress A callback function to report progress of the write operation.
 * @param state Output StreamState to track the number of bytes written and total bytes.
 * @param error Output string to receive an error message if the operation fails.
 * @param error_code Output integer to receive the error code if the operation fails.
 * @return True if the partition data was consumed and written successfully, false otherwise.
 */
bool consume_partition_data(
    const std::function<la_ssize_t(std::uint8_t*, std::size_t, std::string&)>& reader,
    std::uint64_t image_size,
    const std::string& rootfs_device,
    TargetFile& boot_file,
    TargetFile& rootfs_file,
    const WicStreamProgress& progress,
    const WicStreamWriteStart& on_write_start,
    StreamState& state,
    std::string& error,
    int& error_code)
{
    if (image_size < kPartitionTableBufferSize ||
        image_size % kSectorSize != 0 ||
        image_size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        error_code = EINVAL;
        error = "WIC image has an unsupported or invalid size";
        return false;
    }

    std::vector<std::uint8_t> header(kPartitionTableBufferSize);
    if (!read_exact(reader, header.data(), header.size(), error)) {
        error_code = EIO;
        return false;
    }
    std::vector<PartitionRange> parsed_partitions;
    if (!parse_mbr_partitions(header, image_size, parsed_partitions, error)) {
        error_code = EINVAL;
        return false;
    }

    const auto find_partition = [&parsed_partitions](int index) -> const PartitionRange* {
        const auto found = std::find_if(parsed_partitions.begin(), parsed_partitions.end(),
            [index](const PartitionRange& item) { return item.index == index; });
        return found == parsed_partitions.end() ? nullptr : &*found;
    };
    const PartitionRange* const boot_partition = find_partition(kWicBootPartitionNumber);
    const PartitionRange* const rootfs_partition = find_partition(kWicRootfsPartitionNumber);
    if (!boot_partition || !rootfs_partition ||
        boot_partition->start_byte < header.size() ||
        rootfs_partition->start_byte < header.size() ||
        boot_partition->start_byte >= rootfs_partition->start_byte) {
        error_code = EINVAL;
        error = "WIC must contain primary boot and rootfs partitions after its partition table";
        return false;
    }

    std::vector<PartitionRange> targets {*boot_partition, *rootfs_partition};
    std::sort(targets.begin(), targets.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.start_byte < rhs.start_byte;
    });
    std::string opened_target_error;
    if (!create_target_file(*boot_partition, rootfs_partition->byte_length, rootfs_device,
                            boot_file, rootfs_file,
                            opened_target_error, error_code)) {
        error = std::move(opened_target_error);
        return false;
    }
    if (rootfs_partition->byte_length >
        static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        error_code = EOVERFLOW;
        error = "Root filesystem partition size exceeds supported file offsets";
        return false;
    }
    state.total_bytes = boot_partition->byte_length + rootfs_partition->byte_length;

    std::array<std::uint8_t, kChunkSize> buffer {};
    std::uint64_t stream_offset = header.size();
    bool writing_started = false;
    for (const PartitionRange& partition : targets) {
        const std::uint64_t partition_end = partition.start_byte + partition.byte_length;
        if (partition.start_byte < stream_offset || partition_end > image_size) {
            error_code = EINVAL;
            error = "Target partition lies outside the available WIC data";
            return false;
        }
        if (!read_and_discard(reader, partition.start_byte - stream_offset, buffer, error)) {
            error_code = EIO;
            return false;
        }
        stream_offset = partition.start_byte;

        TargetFile& target = partition.index == kWicBootPartitionNumber ? boot_file : rootfs_file;
        std::uint64_t partition_offset = 0;
        std::uint64_t bytes_since_flush = 0;
        while (partition_offset < partition.byte_length) {
            const std::size_t chunk = static_cast<std::size_t>(
                std::min<std::uint64_t>(buffer.size(), partition.byte_length - partition_offset));
            if (!read_exact(reader, buffer.data(), chunk, error)) {
                error_code = EIO;
                return false;
            }
            if (!writing_started) {
                if (on_write_start) {
                    on_write_start();
                }
                writing_started = true;
            }
            if (!write_all_at(target.descriptor.get(), buffer.data(), chunk, partition_offset, error)) {
                error_code = errno == 0 ? EIO : errno;
                return false;
            }
            partition_offset += chunk;
            stream_offset += chunk;
            state.bytes_written += chunk;
            bytes_since_flush += chunk;
            if (target.block_device && bytes_since_flush >= kFlushInterval) {
                if (::fdatasync(target.descriptor.get()) != 0) {
                    error_code = errno;
                    error = std::string("Target block-device flush failed: ") + std::strerror(error_code);
                    return false;
                }
                bytes_since_flush = 0;
            }
            if (progress) {
                progress(state.bytes_written, state.total_bytes);
            }
        }
        if (::fsync(target.descriptor.get()) != 0) {
            error_code = errno;
            error = std::string("Target partition synchronization failed: ") + std::strerror(error_code);
            return false;
        }
    }

    if (stream_offset > image_size ||
        !read_and_discard(reader, image_size - stream_offset, buffer, error)) {
        error_code = EIO;
        return false;
    }
    return true;
}

/**
 * @brief Close a TargetFile's descriptor and handle any errors.
 * @param target The TargetFile whose descriptor should be closed.
 * @param error Output string to receive an error message if closing fails.
 * @param error_code Output integer to receive the error code if closing fails.
 * @return True if the descriptor was closed successfully or was already closed, false otherwise.
 */
bool close_checked(TargetFile& target, std::string& error, int& error_code)
{
    const int descriptor = target.descriptor.release();
    if (descriptor >= 0 && ::close(descriptor) != 0) {
        error_code = errno;
        error = std::string("Failed to close ") + target.path + ": " + std::strerror(error_code);
        return false;
    }
    return true;
}

/**
 * @brief Check if a libarchive entry corresponds to a WIC image file.
 * @param entry The libarchive entry to check.
 * @return True if the entry is a regular file with a .wic extension, false otherwise.
 */
bool is_wic_entry(archive_entry* entry)
{
    if (archive_entry_filetype(entry) != AE_IFREG) {
        return false;
    }
    const char* const pathname = archive_entry_pathname(entry);
    if (!pathname) {
        return false;
    }
    const std::string name(pathname);
    const std::size_t extension_length = sizeof(kWicExtension) - 1;
    return name.size() >= extension_length &&
           name.compare(name.size() - extension_length, extension_length, kWicExtension) == 0;
}

/**
 * @brief Stream a raw WIC image from a file descriptor to the boot and rootfs targets.
 * @param input_descriptor The file descriptor to read the WIC image from.
 * @param image_size The total size of the WIC image in bytes.
 * @param rootfs_device The path to the root filesystem device.
 * @param boot_file Output TargetFile for the temporary boot partition file.
 * @param rootfs_file Output TargetFile for the root filesystem device.
 * @param progress A callback function to report progress of the write operation.
 * @param state Output StreamState to track the number of bytes written and total bytes.
 * @return A WicStreamResult indicating success or failure of the streaming operation.
 */
WicStreamResult stream_raw_image(int input_descriptor,
                                 std::uint64_t image_size,
                                 const std::string& rootfs_device,
                                 TargetFile& boot_file,
                                 TargetFile& rootfs_file,
                                 const WicStreamProgress& progress,
                                 const WicStreamWriteStart& on_write_start,
                                 StreamState& state)
{
    std::string error;
    int error_code = 0;
    const auto reader = [input_descriptor](std::uint8_t* buffer, std::size_t size, std::string& read_error) -> la_ssize_t {
        while (true) {
            const ssize_t result = ::read(input_descriptor, buffer, size);
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result < 0) {
                read_error = std::string("Image read failed: ") + std::strerror(errno);
            }
            return static_cast<la_ssize_t>(result);
        }
    };
    if (!consume_partition_data(reader, image_size, rootfs_device, boot_file, rootfs_file,
                                progress, on_write_start, state, error, error_code)) {
        return failure(error_code, std::move(error));
    }
    std::uint8_t extra_byte = 0;
    const la_ssize_t extra_read = reader(&extra_byte, sizeof(extra_byte), error);
    if (extra_read != 0) {
        return failure(extra_read < 0 ? EIO : EINVAL, "Raw WIC input length does not match its file size");
    }
    return {true, 0, "WIC partitions streamed successfully", boot_file.path,
            state.bytes_written, state.total_bytes};
}

/**
 * @brief Stream a WIC image from a tar.gz archive to the boot and rootfs targets.
 * @param input_descriptor The file descriptor to read the tar.gz archive from.
 * @param rootfs_device The path to the root filesystem device.
 * @param boot_file Output TargetFile for the temporary boot partition file.
 * @param rootfs_file Output TargetFile for the root filesystem device.
 * @param progress A callback function to report progress of the write operation.
 * @param state Output StreamState to track the number of bytes written and total bytes.
 * @return A WicStreamResult indicating success or failure of the streaming operation.
 */
WicStreamResult stream_tar_gz_image(int input_descriptor,
                                    const std::string& rootfs_device,
                                    TargetFile& boot_file,
                                    TargetFile& rootfs_file,
                                    const WicStreamProgress& progress,
                                    const WicStreamWriteStart& on_write_start,
                                    StreamState& state)
{
    std::unique_ptr<struct archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
    if (!reader) {
        return failure(ENOMEM, "Cannot allocate libarchive reader");
    }
    int status = archive_read_support_filter_gzip(reader.get());
    if (status != ARCHIVE_OK) {
        const char* message = archive_error_string(reader.get());
        return failure(EINVAL, std::string("Cannot enable gzip archive filter: ") +
                              (message ? message : "unknown libarchive error"));
    }
    status = archive_read_support_format_tar(reader.get());
    if (status != ARCHIVE_OK) {
        const char* message = archive_error_string(reader.get());
        return failure(EINVAL, std::string("Cannot enable tar archive format: ") +
                              (message ? message : "unknown libarchive error"));
    }
    status = archive_read_open_fd(reader.get(), input_descriptor, kChunkSize);
    if (status != ARCHIVE_OK) {
        const char* message = archive_error_string(reader.get());
        return failure(EIO, std::string("Cannot open OTA archive: ") +
                           (message ? message : "unknown libarchive error"));
    }

    bool found_wic = false;
    std::size_t entry_count = 0;
    std::uint64_t auxiliary_bytes = 0;
    std::string error;
    int error_code = 0;
    WicStreamResult stream_result;
    struct archive_entry* entry = nullptr;

    while (true) {
        status = archive_read_next_header(reader.get(), &entry);
        if (status == ARCHIVE_EOF) {
            break;
        }
        if (status != ARCHIVE_OK) {
            const char* message = archive_error_string(reader.get());
            error = std::string("Failed to read OTA archive header: ") +
                    (message ? message : "unknown libarchive error");
            error_code = EIO;
            break;
        }
        if (++entry_count > kMaxArchiveEntries) {
            error = "OTA archive contains too many entries";
            error_code = EFBIG;
            break;
        }

        if (is_wic_entry(entry)) {
            if (found_wic) {
                error = "OTA archive contains more than one WIC image";
                error_code = EINVAL;
                break;
            }
            if (archive_entry_is_encrypted(entry) > 0 || archive_entry_hardlink(entry) != nullptr) {
                error = "WIC image entry must be an unencrypted regular file";
                error_code = EINVAL;
                break;
            }
            found_wic = true;
            const la_int64_t signed_image_size = archive_entry_size(entry);
            if (!archive_entry_size_is_set(entry) || signed_image_size <= 0 ||
                static_cast<std::uint64_t>(signed_image_size) >
                    static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
                error = "WIC archive entry has an invalid or unsupported size";
                error_code = EINVAL;
                break;
            }
            const auto archive_reader = [archive_handle = reader.get()](
                std::uint8_t* buffer, std::size_t size, std::string& read_error) -> la_ssize_t {
                const la_ssize_t result = archive_read_data(archive_handle, buffer, size);
                if (result < 0) {
                    const char* message = archive_error_string(archive_handle);
                    read_error = std::string("WIC archive data read failed: ") +
                                 (message ? message : "unknown libarchive error");
                }
                return result;
            };
            if (!consume_partition_data(archive_reader, static_cast<std::uint64_t>(signed_image_size),
                                        rootfs_device, boot_file, rootfs_file, progress,
                                        on_write_start, state, error, error_code)) {
                break;
            }
            std::uint8_t extra_byte = 0;
            const la_ssize_t extra_read = archive_reader(&extra_byte, sizeof(extra_byte), error);
            if (extra_read != 0) {
                error = extra_read < 0 ? error : "WIC archive entry exceeds its declared size";
                error_code = extra_read < 0 ? EIO : EINVAL;
                break;
            }
        } else {
            const la_int64_t entry_size = archive_entry_size(entry);
            if (archive_entry_is_encrypted(entry) > 0 || entry_size < 0 ||
                static_cast<std::uint64_t>(entry_size) >
                    kMaxAuxiliaryArchiveBytes - auxiliary_bytes) {
                error = "OTA archive has an encrypted or oversized non-WIC entry";
                error_code = EFBIG;
                break;
            }
            auxiliary_bytes += static_cast<std::uint64_t>(entry_size);
            status = archive_read_data_skip(reader.get());
            if (status != ARCHIVE_OK) {
                const char* message = archive_error_string(reader.get());
                error = std::string("Failed to validate non-WIC archive entry: ") +
                        (message ? message : "unknown libarchive error");
                error_code = EIO;
                break;
            }
        }
    }

    if (error.empty() && !found_wic) {
        error = "OTA archive contains no regular .wic image";
        error_code = ENOENT;
    }
    const int close_status = archive_read_close(reader.get());
    if (error.empty() && close_status != ARCHIVE_OK) {
        const char* message = archive_error_string(reader.get());
        error = std::string("Failed to validate OTA archive trailer: ") +
                (message ? message : "unknown libarchive error");
        error_code = EIO;
    }
    if (!error.empty()) {
        return failure(error_code == 0 ? EIO : error_code, std::move(error));
    }
    stream_result = {true, 0, "WIC partitions streamed successfully", boot_file.path,
                     state.bytes_written, state.total_bytes};
    return stream_result;
}

} // namespace

/**
 * @brief Unmount all mount points of a configured rootfs bank.
 * @param device_path The path to the block device to unmount.
 * @param error Output string to receive an error message if unmounting fails.
 * @param error_code Output integer to receive the error code if unmounting fails.
 * @return True if all mount points were unmounted successfully, false otherwise.
 */
bool unmount_rootfs_bank(const std::string& device_path, std::string& error, int& error_code)
{
    const auto fail = [&error, &error_code](int code, std::string message) {
        error_code = code;
        error = std::move(message);
        return false;
    };

    struct stat device_status {};
    if (::stat(device_path.c_str(), &device_status) != 0) {
        const int stat_error = errno;
        return fail(stat_error, "Cannot inspect " + device_path + ": " + std::strerror(stat_error));
    }
    if (!S_ISBLK(device_status.st_mode)) {
        return fail(ENOTBLK, device_path + " is not a block device");
    }
    const auto matches_configured_bank = [&device_status](const char* configured_device) {
        struct stat configured_status {};
        return ::stat(configured_device, &configured_status) == 0 &&
               S_ISBLK(configured_status.st_mode) &&
               configured_status.st_rdev == device_status.st_rdev;
    };
    if (!matches_configured_bank(FlashConfig::ROOTFS_BANK_A_DEVICE) &&
        !matches_configured_bank(FlashConfig::ROOTFS_BANK_B_DEVICE)) {
        return fail(EPERM, "Refusing to unmount " + device_path +
                           ": it is not a configured rootfs bank");
    }
    struct stat root_status {};
    if (::stat("/", &root_status) != 0) {
        const int stat_error = errno;
        return fail(stat_error, std::string("Cannot inspect the running root filesystem: ") +
                                std::strerror(stat_error));
    }
    if (root_status.st_dev == device_status.st_rdev) {
        return fail(EBUSY, "Refusing to unmount " + device_path + ": it is the running root filesystem");
    }

    // Repeat because unmounting the top mount can expose an earlier mount stacked on the same path.
    for (int pass = 0; pass < kMaximumUnmountPasses; ++pass) {
        std::vector<std::string> mount_points;
        if (!find_mount_points(device_status.st_rdev, mount_points)) {
            return fail(EIO, "Cannot read the mount table to unmount " + device_path);
        }
        if (mount_points.empty()) {
            return true;
        }
        // Newest mounts first, so nested mounts are removed before their parents.
        for (auto mount_point = mount_points.rbegin(); mount_point != mount_points.rend(); ++mount_point) {
            if (*mount_point == "/") {
                return fail(EBUSY, "Refusing to unmount " + device_path + " from /");
            }
            // No MNT_DETACH: a lazy unmount keeps the filesystem live and its writeback would corrupt the new image.
            if (::umount2(mount_point->c_str(), UMOUNT_NOFOLLOW) != 0) {
                const int unmount_error = errno;
                if (unmount_error == EINVAL || unmount_error == ENOENT) {
                    continue;
                }
                return fail(unmount_error, "Cannot unmount " + device_path + " from " + *mount_point +
                                           ": " + std::strerror(unmount_error));
            }
            std::cout << FlashConfig::LOG_TAG << "Unmounted " << device_path << " from "
                      << *mount_point << "." << std::endl;
        }
    }
    return fail(EBUSY, device_path + " is still mounted after repeated unmount attempts");
}

/**
 * @brief Stream a WIC image from a file to the boot and rootfs targets.
 * @param image_path The path to the WIC image file (can be a raw .wic file or a tar.gz archive containing a .wic file).
 * @param rootfs_device The path to the root filesystem device.
 * @param progress A callback function to report progress of the write operation.
 * @return A WicStreamResult indicating success or failure of the streaming operation.
 */
WicStreamResult stream_wic_image(const std::string& image_path,
                                 const std::string& rootfs_device,
                                 const WicStreamProgress& progress,
                                 const WicStreamWriteStart& on_write_start)
{
    if (rootfs_device.empty()) {
        return failure(EINVAL, "Passive root filesystem device is not specified");
    }
    if (rootfs_device != FlashConfig::ROOTFS_BANK_A_DEVICE &&
        rootfs_device != FlashConfig::ROOTFS_BANK_B_DEVICE) {
        return failure(EINVAL, "Root filesystem target is not one of the configured A/B banks");
    }

    UniqueFd input(::open(image_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (input.get() < 0) {
        return failure(errno, std::string("Cannot open OTA image: ") + std::strerror(errno));
    }
    struct stat input_status {};
    if (::fstat(input.get(), &input_status) != 0) {
        return failure(errno, std::string("Cannot inspect OTA image: ") + std::strerror(errno));
    }
    if (!S_ISREG(input_status.st_mode) || input_status.st_size <= 0) {
        return failure(EINVAL, "OTA image is not a non-empty regular file");
    }

    std::array<std::uint8_t, kGzipMagic.size()> signature {};
    ssize_t signature_size = 0;
    while (signature_size < static_cast<ssize_t>(signature.size())) {
        const ssize_t bytes_read = ::pread(input.get(), signature.data() + signature_size,
                                           signature.size() - static_cast<std::size_t>(signature_size),
                                           signature_size);
        if (bytes_read < 0 && errno == EINTR) {
            continue;
        }
        if (bytes_read <= 0) {
            break;
        }
        signature_size += bytes_read;
    }
    if (signature_size != static_cast<ssize_t>(signature.size())) {
        return failure(EIO, "Cannot read OTA image signature");
    }

    TargetFile boot_file;
    TargetFile rootfs_file;
    StreamState state;
    WicStreamResult result;
    if (signature == kGzipMagic) {
        result = stream_tar_gz_image(input.get(), rootfs_device, boot_file, rootfs_file,
                                     progress, on_write_start, state);
    } else {
        result = stream_raw_image(input.get(), static_cast<std::uint64_t>(input_status.st_size),
                                  rootfs_device, boot_file, rootfs_file, progress, on_write_start, state);
    }

    std::string close_error;
    int close_error_code = 0;
    const bool rootfs_closed = close_checked(rootfs_file, close_error, close_error_code);
    const bool boot_closed = close_checked(boot_file, close_error, close_error_code);
    if (!result.success) {
        return result;
    }
    if (!rootfs_closed || !boot_closed) {
        return failure(close_error_code, std::move(close_error));
    }

    result.boot_image_path = boot_file.path;
    boot_file.remove_on_destroy = false;
    return result;
}
