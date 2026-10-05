/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MALI_CSF_FIRMWARE_H
#define MALI_CSF_FIRMWARE_H


#include <stddef.h>
#include <stdint.h>


namespace MaliCSF {

enum FirmwareStatus {
	FIRMWARE_OK = 0,
	FIRMWARE_BAD_DATA,
	FIRMWARE_UNSUPPORTED,
	FIRMWARE_LIMIT_EXCEEDED,
	FIRMWARE_BAD_ARGUMENT
};

enum {
	SECTION_READ = 1 << 0,
	SECTION_WRITE = 1 << 1,
	SECTION_EXECUTE = 1 << 2,
	SECTION_CACHE_MASK = 3 << 3,
	SECTION_PROTECTED = 1 << 5,
	SECTION_SHARED = 1U << 30,
	SECTION_ZERO = 1U << 31
};

// Admission limits for this loader, rather than limits of the file format.
static const size_t kMaxFirmwareBytes = 16 * 1024 * 1024;
static const uint32_t kMaxFirmwareSections = 64;
static const uint64_t kMaxFirmwareMappedBytes = 64 * 1024 * 1024;
static const uint32_t kFirmwareSharedStart = 0x04000000;
static const uint32_t kFirmwareSharedEnd = 0x08000000;

struct FirmwareSection {
	uint32_t flags;
	uint32_t virtualAddress;
	uint32_t memorySize;
	uint32_t dataOffset;
	uint32_t dataSize;
};

struct FirmwareInfo {
	uint32_t versionHash;
	uint32_t tableSize;
	uint32_t sectionCount;
	uint32_t protectedSectionCount;
	uint32_t ignoredEntryCount;
	uint64_t mappedBytes;
	uint8_t major;
	uint8_t minor;
};

// Validated, allocation-free view of an immutable firmware buffer. The caller
// retains the buffer unchanged until the view and all section copies are done.
// This parses the container and initializes CPU memory; it never starts a GPU.
class FirmwareImage {
public:
								FirmwareImage();
	FirmwareStatus			Init(const void* data, size_t size);
	const FirmwareInfo&		Info() const { return fInfo; }
	FirmwareStatus			GetSection(uint32_t index,
									FirmwareSection& section) const;
	FirmwareStatus			CopySection(uint32_t index, void* destination,
									size_t size) const;

private:
	const uint8_t*			fData;
	FirmwareInfo			fInfo;
};

const char* FirmwareStatusName(FirmwareStatus status);

} // namespace MaliCSF

#endif // MALI_CSF_FIRMWARE_H
