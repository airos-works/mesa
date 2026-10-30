/* See summit_parallel_copy.h. */
#include "summit_parallel_copy.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__)
#include <emmintrin.h>
#endif
#ifdef __HAIKU__
#include <OS.h>
#endif

#define SUMMIT_COPY_MAX_THREADS 16
/* Below this a single memcpy is done before helpers would have woken up. */
#define SUMMIT_COPY_PARALLEL_BYTES (2u << 20)
#define SUMMIT_COPY_CHUNK_BYTES (1u << 20)
/* Non-temporal stores only pay when the data does not stay in a cache. */
#define SUMMIT_COPY_STREAM_BYTES (128u << 10)

struct summit_copy_job {
   uint8_t *dst;
   const uint8_t *src;
   ptrdiff_t dst_stride, src_stride;
   size_t row_bytes;
   unsigned rows, rows_per_chunk, chunks;
   bool stream;
   atomic_uint next_chunk;
};

static pthread_once_t summit_copy_once = PTHREAD_ONCE_INIT;
static unsigned summit_copy_helpers;
/* Held by the thread whose copy the helpers are working on. */
static pthread_mutex_t summit_copy_owner = PTHREAD_MUTEX_INITIALIZER;
/* Guards what follows. */
static pthread_mutex_t summit_copy_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t summit_copy_wake = PTHREAD_COND_INITIALIZER;
static pthread_cond_t summit_copy_idle = PTHREAD_COND_INITIALIZER;
static struct summit_copy_job *summit_copy_current;
static unsigned summit_copy_generation;
static unsigned summit_copy_inside;

static void
summit_copy_bytes(uint8_t *dst, const uint8_t *src, size_t bytes, bool stream)
{
#if defined(__x86_64__)
   if (stream && bytes >= 256) {
      size_t head = (size_t)(-(uintptr_t)dst & 15);
      if (head) {
         memcpy(dst, src, head);
         dst += head;
         src += head;
         bytes -= head;
      }
      while (bytes >= 64) {
         __m128i a = _mm_loadu_si128((const __m128i *)(src));
         __m128i b = _mm_loadu_si128((const __m128i *)(src + 16));
         __m128i c = _mm_loadu_si128((const __m128i *)(src + 32));
         __m128i d = _mm_loadu_si128((const __m128i *)(src + 48));
         _mm_stream_si128((__m128i *)(dst), a);
         _mm_stream_si128((__m128i *)(dst + 16), b);
         _mm_stream_si128((__m128i *)(dst + 32), c);
         _mm_stream_si128((__m128i *)(dst + 48), d);
         src += 64;
         dst += 64;
         bytes -= 64;
      }
   }
#else
   (void)stream;
#endif
   if (bytes)
      memcpy(dst, src, bytes);
}

static void
summit_copy_chunks(struct summit_copy_job *job)
{
   for (;;) {
      unsigned chunk = atomic_fetch_add(&job->next_chunk, 1);
      if (chunk >= job->chunks)
         break;
      unsigned first = chunk * job->rows_per_chunk;
      unsigned count = job->rows - first < job->rows_per_chunk ? job->rows - first : job->rows_per_chunk;
      uint8_t *dst = job->dst + (ptrdiff_t)first * job->dst_stride;
      const uint8_t *src = job->src + (ptrdiff_t)first * job->src_stride;
      if (job->dst_stride == job->src_stride && (size_t)job->dst_stride == job->row_bytes)
         summit_copy_bytes(dst, src, job->row_bytes * count, job->stream);
      else {
         for (unsigned row = 0; row < count; row++) {
            summit_copy_bytes(dst, src, job->row_bytes, job->stream);
            dst += job->dst_stride;
            src += job->src_stride;
         }
      }
   }
#if defined(__x86_64__)
   if (job->stream)
      _mm_sfence();
#endif
}

static void *
summit_copy_helper(void *unused)
{
   (void)unused;
#ifdef __HAIKU__
   rename_thread(find_thread(NULL), "frame copy");
   /* The frames are copied for compositor threads, which run at this. */
   set_thread_priority(find_thread(NULL), B_DISPLAY_PRIORITY);
#endif
   unsigned seen = 0;
   pthread_mutex_lock(&summit_copy_lock);
   for (;;) {
      while (seen == summit_copy_generation)
         pthread_cond_wait(&summit_copy_wake, &summit_copy_lock);
      seen = summit_copy_generation;
      struct summit_copy_job *job = summit_copy_current;
      /* Woken too late: the copy is finished, its job is gone. */
      if (!job)
         continue;
      summit_copy_inside++;
      pthread_mutex_unlock(&summit_copy_lock);
      summit_copy_chunks(job);
      pthread_mutex_lock(&summit_copy_lock);
      if (!--summit_copy_inside)
         pthread_cond_broadcast(&summit_copy_idle);
   }
   return NULL;
}

static void
summit_copy_start(void)
{
   unsigned threads = 4;
   const char *value = getenv("SUMMIT_COPY_THREADS");
   if (value && *value)
      threads = (unsigned)atoi(value);
   if (threads > SUMMIT_COPY_MAX_THREADS)
      threads = SUMMIT_COPY_MAX_THREADS;
   for (unsigned i = 1; i < threads; i++) {
      pthread_t thread;
      if (pthread_create(&thread, NULL, summit_copy_helper, NULL))
         break;
      pthread_detach(thread);
      summit_copy_helpers++;
   }
}

void
summit_copy_rows(void *dst, ptrdiff_t dst_stride,
                 const void *src, ptrdiff_t src_stride,
                 size_t row_bytes, unsigned rows)
{
   if (!rows || !row_bytes)
      return;
   size_t total = row_bytes * rows;
   struct summit_copy_job job = {
      .dst = dst, .src = src, .dst_stride = dst_stride, .src_stride = src_stride,
      .row_bytes = row_bytes, .rows = rows,
      .stream = total >= SUMMIT_COPY_STREAM_BYTES,
   };
   job.rows_per_chunk = row_bytes >= SUMMIT_COPY_CHUNK_BYTES ? 1 : (unsigned)(SUMMIT_COPY_CHUNK_BYTES / row_bytes);
   job.chunks = (rows + job.rows_per_chunk - 1) / job.rows_per_chunk;
   atomic_init(&job.next_chunk, 0);

   bool helped = false;
   if (total >= SUMMIT_COPY_PARALLEL_BYTES && job.chunks > 1) {
      pthread_once(&summit_copy_once, summit_copy_start);
      /* Two contexts reading back at once: the second copies alone. */
      if (summit_copy_helpers && !pthread_mutex_trylock(&summit_copy_owner)) {
         helped = true;
         pthread_mutex_lock(&summit_copy_lock);
         summit_copy_current = &job;
         summit_copy_generation++;
         pthread_cond_broadcast(&summit_copy_wake);
         pthread_mutex_unlock(&summit_copy_lock);
      }
   }
   summit_copy_chunks(&job);
   if (helped) {
      /* Every chunk has been taken; wait for the helpers that took some. */
      pthread_mutex_lock(&summit_copy_lock);
      summit_copy_current = NULL;
      while (summit_copy_inside)
         pthread_cond_wait(&summit_copy_idle, &summit_copy_lock);
      pthread_mutex_unlock(&summit_copy_lock);
      pthread_mutex_unlock(&summit_copy_owner);
   }
}
