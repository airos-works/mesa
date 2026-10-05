/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_PROPERTIES_H
#define MALI_CSF_PROPERTIES_H

#include <stdint.h>

namespace MaliCSF {

static const uint32_t kGetQueueProperties = 0x4d435346;
static const uint32_t kClientProperties = 32;
static const uint32_t kQueueReservedRegisters = 4;

// Immutable hardware and validated firmware capabilities, captured by the
// runtime's hardware worker before it accepts queues. The hardware slot counts
// are distinct from maxQueues, the driver's total software queue limit. This
// runtime uses one hardware group/stream at a time. Its wrapper clobbers the
// last reservedRegisters working registers (currently r92..95).
struct GpuProperties {
	uint32_t gpuId, gpuRevision, csfId, coreFeatures;
	uint32_t l2Features, tilerFeatures, memFeatures, mmuFeatures;
	uint32_t threadFeatures, maxThreads, maxWorkgroupSize, maxBarrierSize;
	uint32_t coherencyFeatures, textureFeatures[4], addressSpaces;
	uint64_t shaderPresent, tilerPresent, l2Present, gpuFeatures;
	uint32_t firmwareVersion, firmwareFeatures, groupFeatures, streamFeatures;
	uint32_t groupCount, streamCount, workRegisters, scoreboards;
	uint32_t reservedRegisters, firmwareTimerHz, maxQueues, maxQueueJobs;
	uint64_t userVaLimit;
};

// Requires an owned queue handle. flags and reserved fields must be zero;
// gpu is output only. Copies cached metadata without MMIO, allocations or
// queue work, even if that queue has failed. Use QueueInfo for health. Once
// the handle is retired the query fails. firmwareTimerHz is the frequency
// used for firmware timeouts, not a GPU timestamp/coherency guarantee.
struct QueueProperties {
	uint32_t version, handle, flags, reserved;
	GpuProperties gpu;
	uint64_t reserved2[2];
};

static_assert(sizeof(GpuProperties) == 160, "GPU properties ABI");
static_assert(sizeof(QueueProperties) == 192, "queue properties ABI");

} // namespace MaliCSF
#endif
