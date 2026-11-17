/* Copyright (C) 2001-2019 Peter Selinger.
   This file is part of Potrace. It is free software and it is covered
   by the GNU General Public License. See the file COPYING for details. */

/* Batch tracing with bounded input storage and parallel execution. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_PTHREAD
#include <pthread.h>
#include <unistd.h>
#endif

#include "potracelib.h"
#include "bitmap.h"
#include "curve.h"
#include "auxiliary.h"

#define BATCH_MAX_THREADS 64

typedef struct batch_ctx_s {
  const potrace_param_t *param;
  potrace_bitmap_t *copies;
  potrace_batch_item_t *items;
  const unsigned char *runnable;
  size_t count;
  size_t chunk_size;
  size_t next;
  size_t retained_bytes;
  size_t budget_bytes;
  size_t preallocated_bytes;
#ifdef HAVE_PTHREAD
  pthread_mutex_t lock;
#endif
} batch_ctx_t;

potrace_batch_param_t *potrace_batch_param_default(void) {
  potrace_batch_param_t *p;

  p = (potrace_batch_param_t *)malloc(sizeof(potrace_batch_param_t));
  if (!p) {
    return NULL;
  }
  p->threads = 0;
  p->memory_limit = POTRACE_BATCH_DEFAULT_MEMORY_LIMIT;
  p->chunk_size = POTRACE_BATCH_DEFAULT_CHUNK_SIZE;
  return p;
}

void potrace_batch_param_free(potrace_batch_param_t *p) {
  free(p);
}

static int checked_mul(size_t a, size_t b, size_t *result) {
  if (a != 0 && b > (size_t)-1 / a) {
    return 1;
  }
  *result = a * b;
  return 0;
}

static int checked_add(size_t a, size_t b, size_t *result) {
  if (a > (size_t)-1 - b) {
    return 1;
  }
  *result = a + b;
  return 0;
}

static int bitmap_size(const potrace_bitmap_t *bm, size_t *pixels, size_t *words) {
  size_t wc;
  size_t npixels;
  size_t nwords;
  long dy;

  if (!bm || bm->w < 0 || bm->h < 0) {
    return 1;
  }

  wc = bm->w == 0 ? 0 : ((size_t)bm->w - 1) / BM_WORDBITS + 1;
  if (checked_mul(wc, (size_t)bm->h, &nwords)
      || checked_mul((size_t)bm->w, (size_t)bm->h, &npixels)) {
    return 1;
  }

  if (nwords != 0 && !bm->map) {
    return 1;
  }

  dy = bm->dy;
  if (dy < 0) {
    dy = -dy;
  }
  if (wc != 0 && bm->h != 0 && (size_t)dy < wc) {
    return 1;
  }

  *pixels = npixels;
  *words = nwords;
  return 0;
}

static void fail_item(potrace_batch_item_t *item, int status, int errnum) {
  item->status = status;
  item->errnum = errnum;
  item->state = NULL;
}

static size_t state_bytes(const potrace_state_t *st) {
  const potrace_path_t *p;
  size_t total;
  size_t n;
  size_t n1;

  if (!st) {
    return 0;
  }

  total = sizeof(potrace_state_t);
  for (p = st->plist; p; p = p->next) {
    if (checked_add(total, sizeof(potrace_path_t), &total)
	|| checked_add(total, sizeof(privpath_t), &total)) {
      return (size_t)-1;
    }
    if (!p->priv) {
      continue;
    }

    n = (size_t)p->priv->len;
    if (checked_mul(n, sizeof(point_t), &n1)
	|| checked_add(total, n1, &total)
	|| checked_mul(n, sizeof(int), &n1)
	|| checked_add(total, n1, &total)
	|| checked_mul(n + 1, sizeof(sums_t), &n1)
	|| checked_add(total, n1, &total)
	|| checked_mul((size_t)p->priv->m, sizeof(int), &n1)
	|| checked_add(total, n1, &total)) {
      return (size_t)-1;
    }

    n = (size_t)p->priv->curve.n;
    if (checked_mul(n, sizeof(int), &n1)
	|| checked_add(total, n1, &total)
	|| checked_mul(n, sizeof(potrace_dpoint_t[3]), &n1)
	|| checked_add(total, n1, &total)
	|| checked_mul(n, sizeof(dpoint_t), &n1)
	|| checked_add(total, n1, &total)
	|| checked_mul(n, sizeof(double), &n1)
	|| checked_add(total, n1, &total)
	|| checked_mul(n, sizeof(double), &n1)
	|| checked_add(total, n1, &total)
	|| checked_mul(n, sizeof(double), &n1)
	|| checked_add(total, n1, &total)) {
      return (size_t)-1;
    }

    n = (size_t)p->priv->ocurve.n;
    if (checked_mul(n, sizeof(int), &n1)
	|| checked_add(total, n1, &total)
	|| checked_mul(n, sizeof(potrace_dpoint_t[3]), &n1)
	|| checked_add(total, n1, &total)
	|| checked_mul(n, sizeof(dpoint_t), &n1)
	|| checked_add(total, n1, &total)
	|| checked_mul(n, sizeof(double), &n1)
	|| checked_add(total, n1, &total)
	|| checked_mul(n, sizeof(double), &n1)
	|| checked_add(total, n1, &total)
	|| checked_mul(n, sizeof(double), &n1)
	|| checked_add(total, n1, &total)) {
      return (size_t)-1;
    }
  }

  return total;
}

static int commit_state(batch_ctx_t *ctx, potrace_state_t *st) {
  size_t bytes;
  int fits;

  bytes = state_bytes(st);

#ifdef HAVE_PTHREAD
  pthread_mutex_lock(&ctx->lock);
#endif
  fits = bytes != (size_t)-1 && bytes <= ctx->budget_bytes - ctx->retained_bytes;
  if (fits) {
    ctx->retained_bytes += bytes;
  }
#ifdef HAVE_PTHREAD
  pthread_mutex_unlock(&ctx->lock);
#endif

  return fits;
}

static size_t take_chunk(batch_ctx_t *ctx, size_t *endp) {
  size_t start;
  size_t end;

#ifdef HAVE_PTHREAD
  pthread_mutex_lock(&ctx->lock);
#endif
  start = ctx->next;
  if (start < ctx->count) {
    if (ctx->chunk_size > ctx->count - start) {
      end = ctx->count;
    } else {
      end = start + ctx->chunk_size;
    }
    ctx->next = end;
  } else {
    end = start;
  }
#ifdef HAVE_PTHREAD
  pthread_mutex_unlock(&ctx->lock);
#endif

  *endp = end;
  return start;
}

static void batch_worker(batch_ctx_t *ctx) {
  potrace_param_t local_param;
  potrace_state_t *st;
  potrace_batch_item_t *item;
  size_t start;
  size_t end;
  size_t i;
  int err;

  local_param = *ctx->param;
  local_param.progress.callback = NULL;
  local_param.progress.data = NULL;

  while (1) {
    start = take_chunk(ctx, &end);
    if (start >= end) {
      break;
    }

    for (i = start; i < end; i++) {
      if (!ctx->runnable[i]) {
	continue;
      }

      item = &ctx->items[i];
      errno = 0;
      st = potrace_trace(&local_param, &ctx->copies[i]);
      err = errno;

      if (!st) {
	fail_item(item, POTRACE_BATCH_STATUS_FAILED, err ? err : ENOMEM);
	continue;
      }
      if (!commit_state(ctx, st)) {
	potrace_state_free(st);
	fail_item(item, POTRACE_BATCH_STATUS_NO_MEMORY, ENOMEM);
	continue;
      }

      item->state = st;
      item->status = st->status;
      item->errnum = st->status == POTRACE_STATUS_OK ? 0 : (err ? err : ENOMEM);
    }
  }
}

static void *batch_worker_thread(void *data) {
  batch_worker((batch_ctx_t *)data);
  return NULL;
}

static int automatic_threads(void) {
#ifdef HAVE_PTHREAD
  long n;

  n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n < 1) {
    return POTRACE_BATCH_DEFAULT_THREADS;
  }
  if (n > POTRACE_BATCH_DEFAULT_THREADS) {
    n = POTRACE_BATCH_DEFAULT_THREADS;
  }
  return (int)n;
#else
  return 1;
#endif
}

potrace_batch_result_t *potrace_batch_trace(
  const potrace_param_t *param,
  const potrace_bitmap_t *const *bitmaps,
  size_t count,
  const potrace_batch_param_t *batch_param) {
  potrace_batch_result_t *result;
  batch_ctx_t ctx;
  const potrace_batch_param_t *bp;
  potrace_bitmap_t *copies;
  unsigned char *runnable;
  potrace_word *arena;
  size_t arena_words;
  size_t arena_bytes;
  size_t meta_bytes;
  size_t bytes;
  size_t total_pixels;
  size_t word_offset;
  size_t i;
  size_t y;
  int words;
  int threads;

  if (!param || (count != 0 && !bitmaps)) {
    errno = EINVAL;
    return NULL;
  }

  result = (potrace_batch_result_t *)calloc(1, sizeof(potrace_batch_result_t));
  if (!result) {
    return NULL;
  }
  result->count = count;
  if (count == 0) {
    return result;
  }

  result->items = (potrace_batch_item_t *)calloc(count, sizeof(potrace_batch_item_t));
  copies = (potrace_bitmap_t *)calloc(count, sizeof(potrace_bitmap_t));
  runnable = (unsigned char *)calloc(count, sizeof(unsigned char));
  if (!result->items || !copies || !runnable) {
    free(runnable);
    free(copies);
    free(result->items);
    free(result);
    return NULL;
  }

  if (checked_mul(count, sizeof(potrace_batch_item_t), &meta_bytes)
      || checked_add(meta_bytes, sizeof(potrace_batch_result_t), &meta_bytes)
      || checked_mul(count, sizeof(potrace_bitmap_t), &bytes)
      || checked_add(meta_bytes, bytes, &meta_bytes)
      || checked_add(meta_bytes, count, &meta_bytes)) {
    errno = ENOMEM;
    free(runnable);
    free(copies);
    free(result->items);
    free(result);
    return NULL;
  }

  bp = batch_param;
  arena_words = 0;
  total_pixels = 0;
  for (i = 0; i < count; i++) {
    size_t pixels;
    size_t words_i;

    fail_item(&result->items[i], POTRACE_BATCH_STATUS_INVALID, EINVAL);
    if (bitmap_size(bitmaps[i], &pixels, &words_i)) {
      continue;
    }
    if (checked_add(total_pixels, pixels, &total_pixels)
	|| checked_add(arena_words, words_i, &arena_words)) {
      errno = ENOMEM;
      free(runnable);
      free(copies);
      free(result->items);
      free(result);
      return NULL;
    }
    runnable[i] = 1;
  }
  result->total_pixels = total_pixels;

  if (checked_mul(arena_words, sizeof(potrace_word), &arena_bytes)) {
    arena_bytes = (size_t)-1;
  }
  if (checked_add(meta_bytes, arena_bytes, &bytes)) {
    bytes = (size_t)-1;
  }
  result->preallocated_bytes = bytes;

  if (bytes == (size_t)-1
      || bytes > (bp && bp->memory_limit != 0
		  ? bp->memory_limit : POTRACE_BATCH_DEFAULT_MEMORY_LIMIT)) {
    for (i = 0; i < count; i++) {
      if (runnable[i]) {
	fail_item(&result->items[i], POTRACE_BATCH_STATUS_NO_MEMORY, ENOMEM);
      }
    }
    free(runnable);
    free(copies);
    return result;
  }

  arena = NULL;
  if (arena_words != 0) {
    arena = (potrace_word *)malloc(arena_bytes ? arena_bytes : sizeof(potrace_word));
    if (!arena) {
      for (i = 0; i < count; i++) {
	if (runnable[i]) {
	  fail_item(&result->items[i], POTRACE_BATCH_STATUS_NO_MEMORY, ENOMEM);
	}
      }
      free(runnable);
      free(copies);
      return result;
    }
  }

  word_offset = 0;
  for (i = 0; i < count; i++) {
    const potrace_bitmap_t *src;
    size_t pixels;
    size_t words_i;
    int dy;

    if (!runnable[i]) {
      continue;
    }
    src = bitmaps[i];
    bitmap_size(src, &pixels, &words_i);
    words = src->w == 0 ? 0 : (int)(((size_t)src->w - 1) / BM_WORDBITS + 1);
    copies[i].w = src->w;
    copies[i].h = src->h;
    copies[i].dy = words;
    copies[i].map = arena ? arena + word_offset : NULL;
    dy = words;
    if (dy > 0 && src->h > 0) {
      for (y = 0; y < (size_t)src->h; y++) {
	memcpy(copies[i].map + y * (size_t)dy, bm_scanline(src, (int)y),
	       (size_t)dy * sizeof(potrace_word));
      }
    }
    word_offset += words_i;
  }

  threads = bp ? bp->threads : 0;
  if (threads <= 0) {
    threads = automatic_threads();
  } else if (threads > BATCH_MAX_THREADS) {
    threads = BATCH_MAX_THREADS;
  }
  if ((size_t)threads > count) {
    threads = (int)count;
  }
#ifndef HAVE_PTHREAD
  threads = 1;
#endif

  memset(&ctx, 0, sizeof(ctx));
  ctx.param = param;
  ctx.copies = copies;
  ctx.items = result->items;
  ctx.runnable = runnable;
  ctx.count = count;
  ctx.preallocated_bytes = bytes;
  ctx.budget_bytes = (bp && bp->memory_limit != 0
		      ? bp->memory_limit : POTRACE_BATCH_DEFAULT_MEMORY_LIMIT) - bytes;
  if (bp && bp->chunk_size != 0) {
    ctx.chunk_size = bp->chunk_size;
  } else {
    ctx.chunk_size = count / ((size_t)threads * 8);
    if (ctx.chunk_size == 0) {
      ctx.chunk_size = 1;
    }
  }

#ifdef HAVE_PTHREAD
  {
    pthread_t *tids;
    size_t started;
    int rc;

    pthread_mutex_init(&ctx.lock, NULL);
    tids = threads > 1 ? (pthread_t *)malloc((size_t)(threads - 1) * sizeof(pthread_t)) : NULL;
    started = 0;
    if (tids) {
      for (i = 0; i < (size_t)(threads - 1); i++) {
	rc = pthread_create(&tids[i], NULL, batch_worker_thread, &ctx);
	if (rc != 0) {
	  break;
	}
	started++;
      }
    }

    batch_worker(&ctx);
    for (i = 0; i < started; i++) {
      pthread_join(tids[i], NULL);
    }
    free(tids);
    pthread_mutex_destroy(&ctx.lock);
    result->threads_used = (int)started + 1;
  }
#else
  batch_worker(&ctx);
  result->threads_used = 1;
#endif

  free(arena);
  free(runnable);
  free(copies);
  return result;
}

void potrace_batch_result_free(potrace_batch_result_t *result) {
  size_t i;

  if (!result) {
    return;
  }
  for (i = 0; i < result->count; i++) {
    potrace_state_free(result->items[i].state);
  }
  free(result->items);
  free(result);
}
