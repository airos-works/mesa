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
static const uint32_t kImportBuffer = 0x4d435315;
static const uint32_t kClientCpuBuffers = 1;
static const uint32_t kBufferNormalNoncacheable = 1;
// BufferCreate.flags: the CPU maps this buffer cached (normal write-back
// memory) instead of write-combined. The GPU does not snoop the CPU's caches:
// the client cleans what the CPU wrote before the GPU reads it and cleans and
// invalidates before reading what the GPU wrote (dc cvac / dc civac, which
// user code may run on Haiku arm64). For buffers the CPU reads (read-backs).
static const uint32_t kBufferCpuCached = 2;
// ClientInfo.capabilities: kBufferCpuCached is accepted.
static const uint32_t kClientCachedBuffers = 64;
// ClientInfo.capabilities: kImportBuffer is accepted.
static const uint32_t kClientImportedBuffers = 128;
// BufferImport/BufferInfo.flags: the buffer is memory of the client's team.
static const uint32_t kBufferImported = 4;
// Panfrost keeps one BO per texture, buffer and pool slab and caches freed
// ones, so a WebKit process (compositor plus WebGL) holds thousands of them.
static const uint32_t kMaxClientBuffers = 4096;
static const uint64_t kMaxBufferBytes = UINT64_C(512) << 20;
static const uint64_t kMaxClientBufferBytes = UINT64_C(2) << 30;
// Imported memory stays pinned while the driver holds it: a frame buffer and
// app_server's copy of the screen for each presenting process.
static const uint64_t kMaxClientImportedBytes = UINT64_C(256) << 20;

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

/*	kImportBuffer: a range of the calling team's memory as a buffer, which the
	client maps into its GPU VMs as any other (Summit's direct present renders
	its frames into the frame buffer and app_server's copy of the screen).
	The range lies in one writable area of ordinary memory (no file or device
	mapping), as the team has it: a clone of another team's area is fine.
	The driver holds its pages until the handle is destroyed and no GPU work
	or VM still maps them: a contiguous area's pages through a reference to
	its memory, so that its owner may delete it (a display driver replacing
	its frame buffer); any other range wired in the team. A wired range must
	stay mapped until then (deleting an area waits for it to be unwired); the
	driver lets go of all of it when the descriptor is closed, and when the
	team that imported it exits even if another team still has the descriptor.
	kMapBuffer refuses such a buffer: the team has it mapped already.

	kBufferCpuCached in the output flags: the CPU caches the memory and the
	client keeps it coherent as for its own cached buffers (dc civac the lines
	the GPU will write before submitting, and again after the GPU has written
	them, before the CPU reads). Otherwise it is write-combined or uncached. */
struct BufferImport {
	uint32_t version;
	uint32_t flags; // in: 0; out: kBufferImported, plus kBufferCpuCached
	uint64_t address; // page aligned
	uint64_t bytes; // whole pages
	uint32_t handle; // out
	uint32_t reserved;
};

static_assert(sizeof(ClientInfo) == 72, "client ABI");
static_assert(sizeof(BufferCreate) == 32, "create ABI");
static_assert(sizeof(BufferHandle) == 16, "handle ABI");
static_assert(sizeof(BufferInfo) == 24, "info ABI");
static_assert(sizeof(BufferMap) == 40, "map ABI");
static_assert(sizeof(BufferImport) == 32, "import ABI");

// Each open owns its handles; a duplicated descriptor shares that open. A
// descriptor inherited into another team cannot operate the original client.
// Maps cover the whole buffer and are normal Haiku areas: delete_area() unmaps
// them. They retain the RAM independently of handle destruction / fd close.
// GPU mappings also retain buffers independently of these handles. See CsfVm.h.
// Queue and synchronization operations are defined in CsfQueue.h/CsfSync.h.

} // namespace MaliCSF
#endif
