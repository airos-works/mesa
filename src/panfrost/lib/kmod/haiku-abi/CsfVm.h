/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_VM_H
#define MALI_CSF_VM_H

#include "CsfBuffer.h"

namespace MaliCSF {

static const uint32_t kCreateVm = 0x4d435320;
static const uint32_t kDestroyVm = 0x4d435321;
static const uint32_t kGetVmInfo = 0x4d435322;
static const uint32_t kBindVm = 0x4d435323;
static const uint32_t kClientVmMappings = 2;
static const uint32_t kMaxClientVms = 16;
static const uint32_t kMaxVmMappings = 256;
static const uint32_t kMaxVmOperations = 64;
static const uint32_t kMaxVmTablePages = 1024;
static const uint64_t kVmUserLimit = UINT64_C(1) << 47;
static const uint64_t kMaxVmMappedBytes = UINT64_C(1) << 30;

// Match the permission meanings used by Panthor. Read access is implicit;
// read-only executable, read/write non-executable and read-only non-executable
// mappings are supported. Writable executable mappings are not supported.
static const uint32_t kVmReadOnly = 1;
static const uint32_t kVmNoExecute = 2;
static const uint32_t kVmUncached = 4;
static const uint32_t kVmMapOperation = 0;
static const uint32_t kVmUnmapOperation = 1;

struct VmCreate {
	uint32_t version;
	uint32_t flags;
	uint64_t userLimit; // zero requests the default lower half of the 48-bit VA space
	uint32_t handle;
	uint32_t reserved;
	uint64_t reserved2;
};

struct VmHandle {
	uint32_t version;
	uint32_t handle;
	uint64_t reserved;
};

struct VmInfo {
	uint32_t version;
	uint32_t handle; // zero queries counts without requiring a live VM handle
	uint64_t userLimit;
	uint64_t generation;
	uint64_t mappedBytes;
	uint32_t mappings;
	uint32_t tablePages;
	uint32_t clientVms;
	uint32_t globalVms;
	uint32_t globalGenerations;
	uint32_t globalTablePages;
	uint64_t reserved;
};

// Followed by exactly operationCount VmOperation records. Updates are atomic:
// validation, allocation or output-copy failure leaves the old generation intact.
// Map replaces an overlapping range; unmap may split mappings and ignore holes.
// expectedGeneration=0 accepts the current generation; nonzero is a compare/check.
struct VmBind {
	uint32_t version;
	uint32_t handle;
	uint32_t operationCount;
	uint32_t flags;
	uint64_t expectedGeneration;
	uint64_t newGeneration;
};

struct VmOperation {
	uint32_t type;
	uint32_t flags;
	uint64_t address;
	uint64_t bytes;
	uint32_t buffer;
	uint32_t reserved;
	uint64_t offset;
};

static_assert(sizeof(VmCreate) == 32, "VM create ABI");
static_assert(sizeof(VmHandle) == 16, "VM handle ABI");
static_assert(sizeof(VmInfo) == 64, "VM info ABI");
static_assert(sizeof(VmBind) == 32, "VM bind ABI");
static_assert(sizeof(VmOperation) == 40, "VM operation ABI");

} // namespace MaliCSF
#endif
