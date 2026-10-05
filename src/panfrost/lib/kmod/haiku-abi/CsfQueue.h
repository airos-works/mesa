/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_QUEUE_H
#define MALI_CSF_QUEUE_H

#include "CsfVm.h"
#include "CsfHeap.h"
#include "CsfSync.h"
#include "CsfProperties.h"

namespace MaliCSF {

static const uint32_t kCreateQueue = 0x4d435340;
static const uint32_t kDestroyQueue = 0x4d435341;
static const uint32_t kSubmitQueue = 0x4d435342;
static const uint32_t kWaitQueue = 0x4d435343;
static const uint32_t kGetQueueInfo = 0x4d435344;
static const uint32_t kSubmitQueueSync = 0x4d435345;
static const uint32_t kGetQueueResetInfo = 0x4d435347;
static const uint32_t kClientQueues = 4;
static const uint32_t kMaxRuntimeQueues = 8;
static const uint32_t kMaxQueueJobs = 64;
static const uint32_t kMaxStreamBytes = 1024 * 1024;
static const uint32_t kQueueReady = 1;
static const uint32_t kQueueRunning = 2;
static const uint32_t kQueueFailed = 3;
static const uint32_t kQueueStopping = 4;

static const uint32_t kQueueResetNone = 0;
static const uint32_t kQueueResetLocalError = 1;
static const uint32_t kQueueResetPending = 2;
static const uint32_t kQueueResetQuiescent = 3;
static const uint32_t kQueueResetUnrecoverable = 4;

// Read-only state for an owned queue. state, error and reserved must be zero
// on input. Pending means admission has stopped but device cleanup is still
// running. Quiescent means the failed runtime has finished without retained
// DMA memory; every affected queue must still close before a new runtime can
// start. LocalError affects this queue without a global runtime reset.
struct QueueResetInfo {
	uint32_t version;
	uint32_t handle;
	uint32_t state;
	int32_t error;
	uint64_t reserved[2];
};

// Followed by firmwareBytes bytes for the first queue starting the runtime.
// An already running runtime accepts firmwareBytes=0. Each queue owns a VM
// association and preserves command-stream state between submissions.
struct QueueCreate {
	uint32_t version;
	uint32_t flags;
	uint32_t vm;
	uint32_t firmwareBytes;
	uint32_t handle;
	uint32_t reserved;
	uint64_t generation;
	uint64_t reserved2;
	uint64_t reserved3;
};

struct QueueHandle {
	uint32_t version;
	uint32_t handle;
	uint64_t reserved;
};

// The driver captures a VM generation and retains its mappings before enqueue.
// generation=0 selects current state; nonzero must match. A zero-length stream
// with address=0 is an ordered completion without application commands.
struct QueueSubmit {
	uint32_t version;
	uint32_t handle;
	uint64_t generation;
	uint64_t streamAddress;
	uint32_t streamBytes;
	uint32_t flags;
	uint64_t sequence;
	uint64_t reserved;
};

// Followed by waitCount input SyncPoints, then signalCount output SyncPoints.
// Inputs capture existing fences before outputs are replaced. A missing input
// fence rejects the whole submission. Outputs signal on completion or cancel.
struct QueueSubmitSync {
	QueueSubmit queue;
	uint32_t waitCount, signalCount;
	uint64_t reserved;
};

// timeoutMicros=-1 waits indefinitely, zero polls. An interrupted/timed-out
// wait leaves the job intact. On success, result contains the job status.
struct QueueWait {
	uint32_t version;
	uint32_t handle;
	uint64_t sequence;
	int64_t timeoutMicros;
	int32_t result;
	uint32_t flags;
	uint64_t completed;
};

struct QueueInfo {
	uint32_t version;
	uint32_t handle;
	uint32_t vm;
	uint32_t state;
	int32_t error;
	uint32_t pending;
	uint64_t submitted;
	uint64_t completed;
	uint64_t activeGeneration;
	uint64_t insert;
	uint64_t extract;
	uint64_t failedSequence;
	uint32_t interrupts;
	uint32_t syncEvents;
	uint32_t suspends;
	uint32_t resumes;
	uint64_t reserved;
};

static_assert(sizeof(QueueCreate) == 48, "queue create ABI");
static_assert(sizeof(QueueHandle) == 16, "queue handle ABI");
static_assert(sizeof(QueueSubmit) == 48, "queue submit ABI");
static_assert(sizeof(QueueSubmitSync) == 64, "synchronized queue submit ABI");
static_assert(sizeof(QueueWait) == 40, "queue wait ABI");
static_assert(sizeof(QueueInfo) == 96, "queue info ABI");
static_assert(sizeof(QueueResetInfo) == 32, "queue reset info ABI");

} // namespace MaliCSF
#endif
