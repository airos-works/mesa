/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_SYNC_H
#define MALI_CSF_SYNC_H

#include "CsfBuffer.h"

namespace MaliCSF {
static const uint32_t kCreateSync = 0x4d435350;
static const uint32_t kDestroySync = 0x4d435351;
static const uint32_t kSignalSync = 0x4d435352;
static const uint32_t kResetSync = 0x4d435353;
static const uint32_t kWaitSync = 0x4d435354;
static const uint32_t kGetSyncInfo = 0x4d435355;
static const uint32_t kExportSync = 0x4d435356;
static const uint32_t kImportSync = 0x4d435357;
static const uint32_t kTransferSync = 0x4d435358;
static const uint32_t kWaitSyncFile = 0x4d435359; // on an exported FD
static const uint32_t kClientSynchronization = 8;
static const uint32_t kMaxSyncHandles = 256;
static const uint32_t kMaxSyncPoints = 64;
static const uint32_t kSyncInitiallySignaled = 1;
static const uint32_t kSyncWaitAll = 1;
static const uint32_t kSyncWaitForSubmit = 2;
static const uint32_t kSyncWaitAvailable = 4;
static const uint32_t kSyncSnapshotFd = 1;
static const uint32_t kSyncAvailable = 1;
static const uint32_t kSyncSignaled = 2;

struct SyncCreate {
	uint32_t version, flags, handle, reserved;
};
struct SyncHandle {
	uint32_t version, handle;
	uint64_t reserved;
};
// point=0 selects the current binary fence. Positive timeline insertions must
// increase; a point between two inserted values selects the next higher value.
struct SyncPoint {
	uint32_t handle, flags;
	uint64_t point;
};
// Followed by count SyncPoints. Reset requires point=0. Updates are atomic.
struct SyncBatch {
	uint32_t version, count, flags, reserved;
};
// Followed by count SyncPoints. Waits capture a fence once available, so reset
// or replacement cannot redirect an existing wait. timeout=-1 means indefinite.
// result reports completion/cancellation status; first is the wait-any index.
struct SyncWait {
	uint32_t version, flags, count, first;
	int64_t timeoutMicros;
	int32_t result;
	uint32_t reserved;
};
struct SyncInfo {
	uint32_t version, handle;
	uint64_t submitted, completed;
	uint32_t flags;
	int32_t error;
	uint32_t globalClients, globalObjects, globalPoints, globalEvents;
	uint32_t globalExports, clientHandles;
	uint32_t globalWaits, reserved;
};
// flags=0 shares the object, including future replacements (point must be 0).
// Snapshot exports retain just the selected fence. Shared import creates a new
// handle; snapshot import replaces an existing handle's fence at point.
// Export requires fd=-1 and publishes a close-on-exec FD atomically. dup/fork
// retain it. These are native Haiku descriptors, not Linux sync_file FDs.
struct SyncFd {
	uint32_t version, handle;
	int32_t fd;
	uint32_t flags;
	uint64_t point, reserved;
};
struct SyncTransfer {
	uint32_t version, flags;
	SyncPoint source, destination;
	uint64_t reserved;
};
struct SyncFileWait {
	uint32_t version, flags;
	uint64_t point;
	int64_t timeoutMicros;
	int32_t result;
	uint32_t reserved;
};

static_assert(sizeof(SyncCreate) == 16, "sync create ABI");
static_assert(sizeof(SyncHandle) == 16, "sync handle ABI");
static_assert(sizeof(SyncPoint) == 16, "sync point ABI");
static_assert(sizeof(SyncBatch) == 16, "sync batch ABI");
static_assert(sizeof(SyncWait) == 32, "sync wait ABI");
static_assert(sizeof(SyncInfo) == 64, "sync info ABI");
static_assert(sizeof(SyncFd) == 32, "sync fd ABI");
static_assert(sizeof(SyncTransfer) == 48, "sync transfer ABI");
static_assert(sizeof(SyncFileWait) == 32, "sync file wait ABI");
}
#endif
