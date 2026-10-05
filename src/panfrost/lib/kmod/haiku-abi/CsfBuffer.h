/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_BUFFER_H
#define MALI_CSF_BUFFER_H

#include <stdint.h>

namespace MaliCSF {

// Native Haiku ABI. Call ioctl with the explicit structure size. All reserved
// fields and output fields on create/map must be zero on input.
static const uint32_t kClientVersion = 1;
static const uint32_t kGetClientInfo = 0x4d435310;
static const uint32_t kCreateBuffer = 0x4d435311;
static const uint32_t kDestroyBuffer = 0x4d435312;
static const uint32_t kMapBuffer = 0x4d435313;
static const uint32_t kGetBufferInfo = 0x4d435314;
static const uint32_t kClientCpuBuffers = 1;
static const uint32_t kBufferNormalNoncacheable = 1;
static const uint32_t kMaxClientBuffers = 128;
static const uint64_t kMaxBufferBytes = UINT64_C(64) << 20;
static const uint64_t kMaxClientBufferBytes = UINT64_C(256) << 20;

struct ClientInfo {
	uint32_t version;
	uint32_t capabilities;
	uint32_t maxBuffers;
	uint32_t bufferCount; // resident BOs, including those retained by GPU mappings/work
	uint64_t maxBufferBytes;
	uint64_t maxClientBytes;
	uint64_t bufferBytes;
	uint64_t globalBufferBytes;
	uint32_t globalClients;
	uint32_t globalBuffers;
	uint64_t reserved[2];
};

struct BufferCreate {
	uint32_t version;
	uint32_t flags;
	uint64_t bytes;
	uint32_t handle;
	uint32_t reserved;
	uint64_t reserved2;
};

struct BufferHandle {
	uint32_t version;
	uint32_t handle;
	uint64_t reserved;
};

struct BufferInfo {
	uint32_t version;
	uint32_t handle;
	uint64_t bytes;
	uint32_t flags;
	uint32_t reserved;
};

struct BufferMap {
	uint32_t version;
	uint32_t handle;
	uint64_t address;
	int32_t area;
	uint32_t flags;
	uint64_t bytes;
	uint64_t reserved;
};

static_assert(sizeof(ClientInfo) == 72, "client ABI");
static_assert(sizeof(BufferCreate) == 32, "create ABI");
static_assert(sizeof(BufferHandle) == 16, "handle ABI");
static_assert(sizeof(BufferInfo) == 24, "info ABI");
static_assert(sizeof(BufferMap) == 40, "map ABI");

// Each open owns its handles; a duplicated descriptor shares that open. A
// descriptor inherited into another team cannot operate the original client.
// Maps cover the whole buffer and are normal Haiku areas: delete_area() unmaps
// them. They retain the RAM independently of handle destruction / fd close.
// GPU mappings also retain buffers independently of these handles. See CsfVm.h.
// Queue and synchronization operations are defined in CsfQueue.h/CsfSync.h.

} // namespace MaliCSF
#endif
