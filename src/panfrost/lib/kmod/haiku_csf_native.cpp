/* SPDX-License-Identifier: MIT */
#include "haiku_csf_native.h"
#include "CsfQueue.h"
#include "CsfFirmware.h"
#include <OS.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

using namespace MaliCSF;

static int Fail(int error) { errno = error; return -1; }
static bool TraceEnabled()
{
   const char* value = getenv("HAIKU_CSF_TRACE");
   return value != NULL && strcmp(value, "1") == 0;
}

static int NativeIoctl(int fd, uint32_t op, void* request, size_t bytes)
{
   int result = ioctl(fd, op, request, bytes);
   int saved = errno;
   if (result != 0 && TraceEnabled())
      fprintf(stderr, "HAIKU_MESA_NATIVE_ERROR op=%08x bytes=%zu result=%d errno=%d\n",
         op, bytes, result, saved);
   errno = saved;
   return result;
}

template<typename Request>
static int Call(int fd, uint32_t op, Request& request)
{
   return NativeIoctl(fd, op, &request, sizeof(request));
}

int pan_haiku_check_client(int fd)
{
   ClientInfo info{};
   info.version = kClientVersion;
   if (Call(fd, kGetClientInfo, info) != 0) return -1;
   const uint32_t needed = kClientCpuBuffers | kClientVmMappings | kClientQueues
      | kClientSynchronization | kClientHeaps | kClientProperties;
   if (info.version != kClientVersion || (info.capabilities & needed) != needed)
      return Fail(ENOTSUP);
   return 0;
}

int pan_haiku_buffer_create(int fd, uint64_t bytes, uint32_t* handle)
{
   if (handle == NULL) return Fail(EINVAL);
   BufferCreate request{kClientVersion, 0, bytes, 0, 0, 0};
   if (Call(fd, kCreateBuffer, request) != 0) return -1;
   *handle = request.handle;
   return 0;
}

int pan_haiku_buffer_destroy(int fd, uint32_t handle)
{
   BufferHandle request{kClientVersion, handle, 0};
   return Call(fd, kDestroyBuffer, request);
}

int pan_haiku_buffer_map(int fd, uint32_t handle, pan_haiku_map* map)
{
   if (map == NULL) return Fail(EINVAL);
   BufferMap request{};
   request.version = kClientVersion;
   request.handle = handle;
   if (Call(fd, kMapBuffer, request) != 0) return -1;
   *map = {reinterpret_cast<void*>(uintptr_t(request.address)), request.bytes,
      request.area};
   return 0;
}

int pan_haiku_buffer_unmap(int32_t area)
{
   status_t result = delete_area(area);
   return result == B_OK ? 0 : Fail(B_TO_POSIX_ERROR(result));
}

int pan_haiku_vm_create(int fd, uint64_t limit, uint32_t* handle)
{
   if (handle == NULL) return Fail(EINVAL);
   VmCreate request{kClientVersion, 0, limit, 0, 0, 0};
   if (Call(fd, kCreateVm, request) != 0) return -1;
   *handle = request.handle;
   return 0;
}

int pan_haiku_vm_destroy(int fd, uint32_t handle)
{
   VmHandle request{kClientVersion, handle, 0};
   return Call(fd, kDestroyVm, request);
}

int pan_haiku_vm_generation(int fd, uint32_t vm, uint64_t* generation)
{
   if (generation == NULL) return Fail(EINVAL);
   VmInfo request{};
   request.version = kClientVersion;
   request.handle = vm;
   if (Call(fd, kGetVmInfo, request) != 0) return -1;
   *generation = request.generation;
   return 0;
}

int pan_haiku_vm_bind(int fd, uint32_t vm, const pan_haiku_vm_op* operations,
   uint32_t count, uint64_t expectedGeneration, uint64_t* generation)
{
   if (count == 0 || count > kMaxVmOperations || operations == NULL)
      return Fail(EINVAL);
   struct { VmBind header; VmOperation operations[kMaxVmOperations]; } request{};
   request.header = {kClientVersion, vm, count, 0, expectedGeneration, 0};
   for (uint32_t i = 0; i < count; i++) {
      const pan_haiku_vm_op& op = operations[i];
      request.operations[i] = {op.type, op.flags, op.address, op.bytes,
         op.buffer, 0, op.offset};
   }
   if (NativeIoctl(fd, kBindVm, &request, sizeof(VmBind) + count * sizeof(VmOperation)) != 0)
      return -1;
   if (generation != NULL) *generation = request.header.newGeneration;
   return 0;
}

int pan_haiku_queue_create(int fd, uint32_t vm, const void* firmware,
   size_t bytes, uint32_t* handle)
{
   // Use the pinned native parser admission limit before allocating/copying.
   if (handle == NULL || bytes > kMaxFirmwareBytes
      || (bytes != 0 && firmware == NULL)) return Fail(EINVAL);
   QueueCreate* request = static_cast<QueueCreate*>(calloc(1, sizeof(QueueCreate) + bytes));
   if (request == NULL) return Fail(ENOMEM);
   request->version = kClientVersion;
   request->vm = vm;
   request->firmwareBytes = bytes;
   if (bytes != 0) memcpy(request + 1, firmware, bytes);
   int result = NativeIoctl(fd, kCreateQueue, request, sizeof(QueueCreate) + bytes);
   int error = errno;
   if (result == 0) *handle = request->handle;
   free(request);
   if (result != 0) return Fail(error);
   return 0;
}

int pan_haiku_queue_create_with_file(int fd, uint32_t vm,
   const char* path, uint32_t* handle)
{
   if (handle == NULL) return Fail(EINVAL);
   if (pan_haiku_queue_create(fd, vm, NULL, 0, handle) == 0) return 0;
   if (errno != ENOENT || path == NULL) return -1;
   FILE* file = fopen(path, "rb");
   if (file == NULL) return -1;
   struct stat info;
   int error = 0;
   if (fstat(fileno(file), &info) != 0) error = errno;
   else if (!S_ISREG(info.st_mode) || info.st_size < 20 || uint64_t(info.st_size) > kMaxFirmwareBytes)
      error = EINVAL;
   void* data = NULL;
   if (error == 0) {
      data = malloc(size_t(info.st_size));
      if (data == NULL) error = ENOMEM;
   }
   if (error == 0 && (fread(data, 1, info.st_size, file) != size_t(info.st_size)
      || fgetc(file) != EOF || ferror(file))) error = EIO;
   if (fclose(file) != 0 && error == 0) error = errno;
   if (error != 0) {
      free(data);
      return Fail(error);
   }
   int result = pan_haiku_queue_create(fd, vm, data, info.st_size, handle);
   error = errno;
   free(data);
   // An already-running runtime rejects a nonempty firmware image. Only
   // this EINVAL case after an ENOENT join permits one fresh join attempt.
   if (result != 0 && error == EINVAL)
      return pan_haiku_queue_create(fd, vm, NULL, 0, handle);
   return result == 0 ? 0 : Fail(error);
}

int pan_haiku_queue_destroy(int fd, uint32_t handle)
{
   if (TraceEnabled()) {
      int saved = errno;
      QueueInfo info{};
      info.version = kClientVersion;
      info.handle = handle;
      int result = Call(fd, kGetQueueInfo, info);
      fprintf(stderr, "HAIKU_MESA_NATIVE_QUEUE handle=%u query=%d state=%u error=%d pending=%u submitted=%llu completed=%llu failed=%llu\n",
         handle, result, info.state, info.error, info.pending,
         (unsigned long long)info.submitted, (unsigned long long)info.completed,
         (unsigned long long)info.failedSequence);
      errno = saved;
   }
   QueueHandle request{kClientVersion, handle, 0};
   return Call(fd, kDestroyQueue, request);
}

int pan_haiku_queue_properties(int fd, uint32_t queue, pan_haiku_properties* properties)
{
   if (properties == NULL) return Fail(EINVAL);
   QueueProperties request{};
   request.version = kClientVersion;
   request.handle = queue;
   if (Call(fd, kGetQueueProperties, request) != 0) return -1;
   const GpuProperties& gpu = request.gpu;
   if (TraceEnabled()) {
      int saved = errno;
      fprintf(stderr, "HAIKU_MESA_NATIVE_GPU handle=%u id=%08x shader=%llx firmware=%08x csf=%08x registers=%u reserved=%u\n",
         queue, gpu.gpuId, (unsigned long long)gpu.shaderPresent,
         gpu.firmwareVersion, gpu.csfId, gpu.workRegisters, gpu.reservedRegisters);
      errno = saved;
   }
   pan_haiku_properties result{};
   result.gpu_id = gpu.gpuId;
   result.gpu_revision = gpu.gpuRevision;
   result.core_features = gpu.coreFeatures;
   result.tiler_features = gpu.tilerFeatures;
   result.mem_features = gpu.memFeatures;
   result.mmu_features = gpu.mmuFeatures;
   for (unsigned i = 0; i < 4; i++) result.texture_features[i] = gpu.textureFeatures[i];
   result.max_threads = gpu.maxThreads;
   result.max_workgroup_size = gpu.maxWorkgroupSize;
   result.thread_features = gpu.threadFeatures;
   result.shader_present = gpu.shaderPresent;
   result.tiler_present = gpu.tilerPresent;
   result.user_va_limit = gpu.userVaLimit;
   result.group_count = gpu.groupCount;
   result.stream_count = gpu.streamCount;
   result.work_registers = gpu.workRegisters;
   result.scoreboards = gpu.scoreboards;
   result.reserved_registers = gpu.reservedRegisters;
   result.max_queues = gpu.maxQueues;
   result.max_queue_jobs = gpu.maxQueueJobs;
   *properties = result;
   return 0;
}

int pan_haiku_queue_info(int fd, uint32_t queue, pan_haiku_queue_state* state)
{
   if (state == NULL) return Fail(EINVAL);
   QueueInfo request{};
   request.version = kClientVersion;
   request.handle = queue;
   if (Call(fd, kGetQueueInfo, request) != 0) return -1;
   *state = {request.vm, request.state, request.pending, request.error,
      request.submitted, request.completed, request.failedSequence};
   return 0;
}

static_assert(PAN_HAIKU_RESET_NONE == kQueueResetNone &&
   PAN_HAIKU_RESET_LOCAL_ERROR == kQueueResetLocalError &&
   PAN_HAIKU_RESET_PENDING == kQueueResetPending &&
   PAN_HAIKU_RESET_QUIESCENT == kQueueResetQuiescent &&
   PAN_HAIKU_RESET_UNRECOVERABLE == kQueueResetUnrecoverable, "native reset states");

int pan_haiku_queue_reset_info(int fd, uint32_t queue, pan_haiku_reset_info* info)
{
   if (info == NULL) return Fail(EINVAL);
   QueueResetInfo request{};
   request.version = kClientVersion;
   request.handle = queue;
   if (Call(fd, kGetQueueResetInfo, request) != 0) return -1;
   if (request.state > kQueueResetUnrecoverable
       || (request.state == kQueueResetNone) != (request.error == B_OK))
      return Fail(EIO);
   *info = {request.state, request.error};
   if (TraceEnabled()) {
      int saved = errno;
      fprintf(stderr, "HAIKU_MESA_NATIVE_RESET handle=%u state=%u error=%d\n",
         queue, request.state, request.error);
      errno = saved;
   }
   return 0;
}

int pan_haiku_queue_submit(int fd, uint32_t queue, uint64_t generation,
   uint64_t address, uint32_t bytes, const pan_haiku_sync_point* waits,
   uint32_t waitCount, const pan_haiku_sync_point* signals, uint32_t signalCount,
   uint64_t* sequence)
{
   if (waitCount > kMaxSyncPoints || signalCount > kMaxSyncPoints - waitCount
      || (waitCount != 0 && waits == NULL) || (signalCount != 0 && signals == NULL))
      return Fail(EINVAL);
   struct { QueueSubmitSync header; SyncPoint points[kMaxSyncPoints]; } request{};
   request.header.queue = {kClientVersion, queue, generation, address, bytes, 0, 0, 0};
   request.header.waitCount = waitCount;
   request.header.signalCount = signalCount;
   for (uint32_t i = 0; i < waitCount; i++)
      request.points[i] = {waits[i].handle, 0, waits[i].point};
   for (uint32_t i = 0; i < signalCount; i++)
      request.points[waitCount + i] = {signals[i].handle, 0, signals[i].point};
   __sync_synchronize();
   if (NativeIoctl(fd, kSubmitQueueSync, &request,
      sizeof(QueueSubmitSync) + (waitCount + signalCount) * sizeof(SyncPoint)) != 0)
      return -1;
   if (TraceEnabled()) {
      int saved = errno;
      fprintf(stderr, "HAIKU_MESA_NATIVE_SUBMIT handle=%u generation=%llu address=%llx bytes=%u waits=%u signals=%u sequence=%llu\n",
         queue, (unsigned long long)request.header.queue.generation,
         (unsigned long long)address, bytes, waitCount, signalCount,
         (unsigned long long)request.header.queue.sequence);
      errno = saved;
   }
   if (sequence != NULL) *sequence = request.header.queue.sequence;
   return 0;
}

int pan_haiku_heap_create(int fd, uint32_t vm, uint32_t chunkSize,
   uint32_t initialChunks, uint32_t maxChunks, uint32_t targetInFlight,
   pan_haiku_heap* heap)
{
   if (heap == NULL) return Fail(EINVAL);
   HeapCreate request{kClientVersion, 0, vm, chunkSize,
      initialChunks, maxChunks, targetInFlight, 0, 0, 0, 0, 0};
   if (Call(fd, kCreateHeap, request) != 0) return -1;
   *heap = {request.handle, request.contextAddress, request.firstChunkAddress};
   return 0;
}

int pan_haiku_heap_destroy(int fd, uint32_t vm, uint32_t heap)
{
   HeapHandle request{kClientVersion, vm, heap, 0, 0, 0};
   return Call(fd, kDestroyHeap, request);
}
