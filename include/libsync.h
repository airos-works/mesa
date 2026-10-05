/*
 * Copyright 2026, air/OS. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

/* air/OS (Haiku): there are no sync files. The V3D device runs jobs in
 * submission order, so a job never has to wait for a fence explicitly; see
 * include/xf86drm.h. These keep v3d's fence import paths compiling. */
#ifndef HAIKU_V3D_LIBSYNC_H
#define HAIKU_V3D_LIBSYNC_H

#ifndef __HAIKU__
#error This libsync.h is the air/OS stand-in.
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline int
sync_wait(int fd, int timeout)
{
   (void)fd; (void)timeout;
   return 0;
}

static inline int
sync_accumulate(const char *name, int *fd1, int fd2)
{
   (void)name;
   *fd1 = fd2;
   return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* HAIKU_V3D_LIBSYNC_H */
