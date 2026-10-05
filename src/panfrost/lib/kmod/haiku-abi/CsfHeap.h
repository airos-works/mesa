/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_HEAP_H
#define MALI_CSF_HEAP_H

#include "CsfVm.h"

namespace MaliCSF {

static const uint32_t kCreateHeap = 0x4d435360;
static const uint32_t kDestroyHeap = 0x4d435361;
static const uint32_t kGetHeapInfo = 0x4d435362;
static const uint32_t kClientHeaps = 16;
static const uint32_t kMaxVmHeaps = 128;
static const uint32_t kMaxHeapChunks = 64;
static const uint64_t kMaxClientHeapBytes = UINT64_C(256) << 20;
static const uint32_t kHeapRootIndex = 257;
static const uint64_t kHeapBase = uint64_t(kHeapRootIndex) << 39;
static const uint64_t kHeapSlotBytes = UINT64_C(1) << 30;
static const uint64_t kHeapChunksOffset = UINT64_C(2) << 20;

// Kernel-owned, GPU RW/NX memory. No CPU buffer handle or CPU mapping is exposed.
// Creation/destruction publishes a new VM generation only after checked copyout.
// Older queued generations retain the heap, including after all public handles
// disappear. The 32-byte opaque GPU context has a private, zeroed 4 KiB page.
struct HeapCreate {
	uint32_t version, flags, vm, chunkSize;
	uint32_t initialChunks, maxChunks, targetInFlight, handle;
	uint64_t contextAddress, firstChunkAddress, generation, reserved;
};

struct HeapHandle {
	uint32_t version, vm, handle, reserved;
	uint64_t generation, reserved2;
};

// vm=handle=0 queries resident counts, including retired heaps and reservations
// for pending growth. bytes includes the context page and resident chunk data;
// page-table memory is counted separately. A live heap query requires both IDs.
struct HeapInfo {
	uint32_t version, vm, handle, reserved;
	uint64_t contextAddress, firstChunkAddress, bytes;
	uint32_t chunkSize, initialChunks, maxChunks, chunkCount, targetInFlight, slot;
	uint64_t generation;
	uint32_t clientHeaps, globalHeaps, globalChunks, globalTablePages;
	uint64_t clientBytes, globalBytes;
	uint32_t globalHeapGenerations, reserved2;
};

static_assert(sizeof(HeapCreate) == 64, "heap create ABI");
static_assert(sizeof(HeapHandle) == 32, "heap destroy ABI");
static_assert(sizeof(HeapInfo) == 112, "heap info ABI");

} // namespace MaliCSF
#endif
