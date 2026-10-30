/*
 * Summit: copying a frame that was read back from the GPU, or is about to be
 * uploaded, with several threads.
 *
 * A 3840x1756 frame is 27 MB. One thread copies it in about 4 ms, which is
 * the larger half of a glReadPixels of that frame on zink, and Summit reads
 * one back for every frame it shows. The copy is split into chunks of about
 * a megabyte that the calling thread and a few helpers take in turn, written
 * with non-temporal stores: the destination is far larger than the caches
 * and its reader is another process.
 *
 * SUMMIT_COPY_THREADS sets the number of threads that copy, the caller
 * included (default 4, 1 turns the helpers off).
 */
#ifndef SUMMIT_PARALLEL_COPY_H
#define SUMMIT_PARALLEL_COPY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void
summit_copy_rows(void *dst, ptrdiff_t dst_stride,
                 const void *src, ptrdiff_t src_stride,
                 size_t row_bytes, unsigned rows);

#ifdef __cplusplus
}
#endif

#endif
