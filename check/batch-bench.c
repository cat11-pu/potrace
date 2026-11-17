/* Copyright (C) 2001-2019 Peter Selinger.
   This file is part of Potrace. It is free software and it is covered
   by the GNU General Public License. See the file COPYING for details. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/resource.h>
#endif

#include "potracelib.h"

#define BM_WORDSIZE ((int)sizeof(potrace_word))
#define BM_WORDBITS (8 * BM_WORDSIZE)
#define BM_HIBIT (((potrace_word)1) << (BM_WORDBITS - 1))
#define bm_scanline(bm, y) ((bm)->map + (y) * (bm)->dy)
#define bm_index(bm, x, y) (&bm_scanline(bm, y)[(x) / BM_WORDBITS])
#define bm_mask(x) (BM_HIBIT >> ((x) & (BM_WORDBITS - 1)))

static double now(void) {
#if defined(_WIN32)
  LARGE_INTEGER freq;
  LARGE_INTEGER value;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&value);
  return (double)value.QuadPart / (double)freq.QuadPart;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

static size_t peak_rss_bytes(void) {
#if defined(_WIN32)
  return 0;
#else
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
  return (size_t)ru.ru_maxrss;
#else
  return (size_t)ru.ru_maxrss * 1024;
#endif
#endif
}

static potrace_bitmap_t *bitmap_new(int w, int h) {
  potrace_bitmap_t *bm;
  int dy;

  bm = (potrace_bitmap_t *)malloc(sizeof(potrace_bitmap_t));
  if (!bm) {
    return NULL;
  }
  dy = (w - 1) / BM_WORDBITS + 1;
  bm->w = w;
  bm->h = h;
  bm->dy = dy;
  bm->map = (potrace_word *)calloc((size_t)dy * (size_t)h,
				   sizeof(potrace_word));
  if (!bm->map) {
    free(bm);
    return NULL;
  }
  return bm;
}

static void bitmap_free(potrace_bitmap_t *bm) {
  if (bm) {
    free(bm->map);
    free(bm);
  }
}

static void bitmap_put(potrace_bitmap_t *bm, int x, int y, int value) {
  potrace_word *p = bm_index(bm, x, y);

  if (value) {
    *p |= bm_mask(x);
  } else {
    *p &= ~bm_mask(x);
  }
}

static void make_icon(potrace_bitmap_t *bm, int image) {
  int x;
  int y;
  int cx = 256 + (image * 37) % 80 - 40;
  int cy = 256 + (image * 53) % 80 - 40;
  int radius = 120 + image % 40;
  int hole = radius - 25;
  int dotx = 80 + image % 7;
  int doty = 80;

  for (y = 0; y < bm->h; y++) {
    for (x = 0; x < bm->w; x++) {
      int dx = x - cx;
      int dy = y - cy;
      int d = dx * dx + dy * dy;
      int dotdx = x - dotx;
      int dotdy = y - doty;

      if (d < radius * radius && d > hole * hole) {
	bitmap_put(bm, x, y, 1);
      }
      if (dotdx * dotdx + dotdy * dotdy < 35 * 35) {
	bitmap_put(bm, x, y, 1);
      }
      if (x > 300 && x < 420 && y > 330 && y < 350) {
	bitmap_put(bm, x, y, 1);
      }
    }
  }
}

static void hash_bytes(uint64_t *hash, const void *data, size_t size) {
  const unsigned char *p = (const unsigned char *)data;
  size_t i;

  for (i = 0; i < size; i++) {
    *hash ^= p[i];
    *hash *= UINT64_C(1099511628211);
  }
}

static uint64_t hash_state(const potrace_state_t *st) {
  const potrace_path_t *p;
  uint64_t hash = UINT64_C(1469598103934665603);
  int i;

  hash_bytes(&hash, &st->status, sizeof(st->status));
  for (p = st->plist; p; p = p->next) {
    hash_bytes(&hash, &p->area, sizeof(p->area));
    hash_bytes(&hash, &p->sign, sizeof(p->sign));
    hash_bytes(&hash, &p->curve.n, sizeof(p->curve.n));
    for (i = 0; i < p->curve.n; i++) {
      hash_bytes(&hash, &p->curve.tag[i], sizeof(p->curve.tag[i]));
      if (p->curve.tag[i] == POTRACE_CORNER) {
	hash_bytes(&hash, &p->curve.c[i][1], sizeof(potrace_dpoint_t));
	hash_bytes(&hash, &p->curve.c[i][2], sizeof(potrace_dpoint_t));
      } else {
	hash_bytes(&hash, &p->curve.c[i][0], sizeof(potrace_dpoint_t));
	hash_bytes(&hash, &p->curve.c[i][1], sizeof(potrace_dpoint_t));
	hash_bytes(&hash, &p->curve.c[i][2], sizeof(potrace_dpoint_t));
      }
    }
  }
  return hash;
}

int main(int argc, char **argv) {
  int count = argc > 1 ? atoi(argv[1]) : 1000;
  int threads = argc > 2 ? atoi(argv[2]) : 8;
  int chunk = argc > 3 ? atoi(argv[3]) : 0;
  int width = 512;
  int height = 512;
  potrace_param_t *param;
  potrace_batch_param_t *bp;
  potrace_batch_result_t *batch;
  potrace_bitmap_t **bitmaps;
  uint64_t *direct_hash;
  uint64_t *batch_hash;
  size_t *path_counts;
  size_t paths = 0;
  double t0;
  double sequential;
  double parallel;
  double reduction;
  double speedup;
  size_t rss;
  size_t mismatches = 0;
  int failures = 0;
  int i;

  if (count < 1) {
    fprintf(stderr, "usage: %s [count>=1] [threads>=1] [chunk]\n", argv[0]);
    return 2;
  }

  param = potrace_param_default();
  bp = potrace_batch_param_default();
  bitmaps = (potrace_bitmap_t **)calloc((size_t)count, sizeof(*bitmaps));
  direct_hash = (uint64_t *)calloc((size_t)count, sizeof(*direct_hash));
  batch_hash = (uint64_t *)calloc((size_t)count, sizeof(*batch_hash));
  path_counts = (size_t *)calloc((size_t)count, sizeof(*path_counts));
  if (!param || !bp || !bitmaps || !direct_hash || !batch_hash || !path_counts) {
    fprintf(stderr, "allocation failed: %s\n", strerror(errno));
    return 1;
  }
  param->turdsize = 2;
  bp->threads = threads;
  bp->chunk_size = (size_t)chunk;
  bp->memory_limit = POTRACE_BATCH_DEFAULT_MEMORY_LIMIT;

  for (i = 0; i < count; i++) {
    bitmaps[i] = bitmap_new(width, height);
    if (!bitmaps[i]) {
      fprintf(stderr, "bitmap allocation failed at %d: %s\n", i, strerror(errno));
      return 1;
    }
    make_icon(bitmaps[i], i);
  }

  t0 = now();
  for (i = 0; i < count; i++) {
    potrace_state_t *st = potrace_trace(param, bitmaps[i]);
    const potrace_path_t *p;

    if (!st || st->status != POTRACE_STATUS_OK) {
      fprintf(stderr, "sequential trace failed at %d\n", i);
      return 1;
    }
    direct_hash[i] = hash_state(st);
    for (p = st->plist; p; p = p->next) {
      path_counts[i]++;
    }
    paths += path_counts[i];
    potrace_state_free(st);
  }
  sequential = now() - t0;

  t0 = now();
  batch = potrace_batch_trace(param,
			      (const potrace_bitmap_t *const *)bitmaps,
			      (size_t)count, bp);
  parallel = now() - t0;
  if (!batch) {
    fprintf(stderr, "batch trace failed: %s\n", strerror(errno));
    return 1;
  }

  for (i = 0; i < count; i++) {
    potrace_batch_item_t *item = &batch->items[i];

    if (item->status != POTRACE_STATUS_OK || !item->state) {
      fprintf(stderr, "batch item %d failed: status=%d errno=%d\n",
	      i, item->status, item->errnum);
      failures++;
      continue;
    }
    batch_hash[i] = hash_state(item->state);
    if (direct_hash[i] != batch_hash[i]) {
      mismatches++;
    }
  }

  rss = peak_rss_bytes();
  reduction = sequential > 0.0 ? 1.0 - parallel / sequential : 0.0;
  speedup = parallel > 0.0 ? sequential / parallel : 0.0;

  printf("threads=%d images=%d size=%dx%d turdsize=%d chunk=%s\n",
	 batch->threads_used, count, width, height, param->turdsize,
	 chunk == 0 ? "auto" : argv[3]);
  printf("sequential=%.4fs batch=%.4fs speedup=%.2fx time_reduction=%.1f%%\n",
	 sequential, parallel, speedup, reduction * 100.0);
  printf("paths=%lu exact_matches=%d/%d mismatches=%lu\n",
	 (unsigned long)paths, count - (int)mismatches, count,
	 (unsigned long)mismatches);
  printf("preallocated=%.1fMiB peak_rss=%.1fMiB cap=%.1fMiB\n",
	 (double)batch->preallocated_bytes / 1048576.0,
	 (double)rss / 1048576.0,
	 (double)bp->memory_limit / 1048576.0);

  if (mismatches != 0 || failures != 0) {
    fprintf(stderr, "FAIL: batch results differ from per-image calls\n");
    failures++;
  }
  if (rss > bp->memory_limit) {
    fprintf(stderr, "FAIL: peak RSS exceeds the memory limit\n");
    failures++;
  }
  if (batch->threads_used >= 8 && count >= 1000 && reduction < 0.40) {
    fprintf(stderr, "FAIL: time reduction is below 40%%\n");
    failures++;
  }

  potrace_batch_result_free(batch);
  for (i = 0; i < count; i++) {
    bitmap_free(bitmaps[i]);
  }
  free(bitmaps);
  free(direct_hash);
  free(batch_hash);
  free(path_counts);
  potrace_param_free(param);
  potrace_batch_param_free(bp);

  if (failures) {
    return 1;
  }
  printf("batch-bench: PASS\n");
  return 0;
}
