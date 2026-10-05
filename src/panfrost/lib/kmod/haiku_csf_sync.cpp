/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include "haiku_csf_sync.h"
#include "CsfSync.h"
#include <OS.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <time.h>

using namespace MaliCSF;

static int Fail(int error) { errno = error; return -1; }

template<typename Request>
static int Call(int fd, uint32_t op, Request& request)
{
   return ioctl(fd, op, &request, sizeof(request));
}

int pan_haiku_sync_create(int fd, uint32_t flags, uint32_t* handle)
{
   if (handle == NULL) return Fail(EINVAL);
   SyncCreate request{kClientVersion, flags, 0, 0};
   if (flags == kSyncInitiallySignaled) __sync_synchronize();
   if (Call(fd, kCreateSync, request) != 0) return -1;
   *handle = request.handle;
   return 0;
}

int pan_haiku_sync_destroy(int fd, uint32_t handle)
{
   SyncHandle request{kClientVersion, handle, 0};
   return Call(fd, kDestroySync, request);
}

int pan_haiku_sync_update(int fd, const uint32_t* handles,
   const uint64_t* points, uint32_t count, int reset)
{
   if (count == 0 || count > kMaxSyncPoints || handles == NULL
      || (reset != 0 && reset != 1)) return Fail(EINVAL);
   struct { SyncBatch header; SyncPoint points[kMaxSyncPoints]; } request{};
   request.header = {kClientVersion, count, 0, 0};
   for (unsigned i = 0; i < count; i++)
      request.points[i] = {handles[i], 0, points != NULL ? points[i] : 0};
   if (!reset) __sync_synchronize();
   return ioctl(fd, reset ? kResetSync : kSignalSync, &request,
      sizeof(SyncBatch) + count * sizeof(SyncPoint));
}

int pan_haiku_sync_transfer(int fd, uint32_t destination, uint64_t destinationPoint,
   uint32_t source, uint64_t sourcePoint, uint32_t flags)
{
   SyncTransfer request{kClientVersion, flags,
      {source, 0, sourcePoint}, {destination, 0, destinationPoint}, 0};
   return Call(fd, kTransferSync, request);
}

static int RemainingMicros(int64_t deadline, int64_t& result)
{
   if (deadline == INT64_MAX) { result = -1; return 0; }
   if (deadline <= 0) { result = 0; return 0; }
   timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
   if (now.tv_sec < 0 || now.tv_nsec < 0 || now.tv_nsec >= 1000000000)
      return Fail(EIO);
   // Avoid overflow even for a clock outside the representable deadline range.
   if (uint64_t(now.tv_sec) > uint64_t(INT64_MAX) / 1000000000) {
      result = 0; return 0;
   }
   uint64_t time = uint64_t(now.tv_sec) * 1000000000 + uint64_t(now.tv_nsec);
   if (time >= uint64_t(deadline)) { result = 0; return 0; }
   uint64_t delta = uint64_t(deadline) - time;
   result = delta / 1000 + (delta % 1000 != 0);
   return 0;
}

int pan_haiku_sync_wait(int fd, const uint32_t* handles, const uint64_t* points,
   uint32_t count, int64_t deadline, uint32_t flags, uint32_t* first)
{
   if (count == 0 || count > kMaxSyncPoints || handles == NULL
      || (flags & ~(kSyncWaitAll | kSyncWaitForSubmit | kSyncWaitAvailable)) != 0)
      return Fail(EINVAL);
   struct { SyncWait header; SyncPoint points[kMaxSyncPoints]; } request{};
   request.header = {kClientVersion, flags, count, 0, 0, 0, 0};
   for (unsigned i = 0; i < count; i++)
      request.points[i] = {handles[i], 0, points != NULL ? points[i] : 0};
   if (RemainingMicros(deadline, request.header.timeoutMicros) != 0) return -1;
   if (ioctl(fd, kWaitSync, &request, sizeof(SyncWait) + count * sizeof(SyncPoint)) != 0)
      return -1;
   if (request.header.result != B_OK)
      return Fail(B_TO_POSIX_ERROR(request.header.result));
   __sync_synchronize();
   if (first != NULL) *first = request.header.first;
   return 0;
}

int pan_haiku_sync_export(int fd, uint32_t handle, uint64_t point,
   int snapshot, int* exportedFd)
{
   if (exportedFd == NULL || (snapshot != 0 && snapshot != 1)) return Fail(EINVAL);
   SyncFd request{kClientVersion, handle, -1, snapshot ? kSyncSnapshotFd : 0, point, 0};
   if (Call(fd, kExportSync, request) != 0) return -1;
   *exportedFd = request.fd;
   return 0;
}

int pan_haiku_sync_import(int fd, int sharedFd, uint32_t* handle)
{
   if (handle == NULL) return Fail(EINVAL);
   SyncFd request{kClientVersion, 0, sharedFd, 0, 0, 0};
   if (Call(fd, kImportSync, request) != 0) return -1;
   *handle = request.handle;
   return 0;
}

int pan_haiku_sync_import_snapshot(int fd, uint32_t handle, uint64_t point,
   int snapshotFd)
{
   SyncFd request{kClientVersion, handle, snapshotFd, kSyncSnapshotFd, point, 0};
   return Call(fd, kImportSync, request);
}
