/* Copyright (C) 2001-2019 Peter Selinger.
   This file is part of Potrace. It is free software and it is covered
   by the GNU General Public License. See the file COPYING for details. */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "potracelib.h"

#define BM_WORDSIZE ((int)sizeof(potrace_word))
#define BM_WORDBITS (8 * BM_WORDSIZE)
#define BM_HIBIT (((potrace_word)1) << (BM_WORDBITS - 1))
#define bm_scanline(bm, y) ((bm)->map + (y) * (bm)->dy)
#define bm_index(bm, x, y) (&bm_scanline(bm, y)[(x) / BM_WORDBITS])
#define bm_mask(x) (BM_HIBIT >> ((x) & (BM_WORDBITS - 1)))

static potrace_bitmap_t *bitmap_new(int w, int h) {
  potrace_bitmap_t *bm;
  int dy;

  bm = (potrace_bitmap_t *)malloc(sizeof(potrace_bitmap_t));
  if (!bm) {
    return NULL;
  }
  dy = w == 0 ? 0 : (w - 1) / BM_WORDBITS + 1;
  bm->w = w;
  bm->h = h;
  bm->dy = dy;
  bm->map = (potrace_word *)calloc((size_t)(dy ? dy : 1) * (size_t)(h ? h : 1),
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

static void fill_rect(potrace_bitmap_t *bm, int x0, int y0, int x1, int y1,
		      int value) {
  int x;
  int y;

  for (y = y0; y < y1; y++) {
    for (x = x0; x < x1; x++) {
      bitmap_put(bm, x, y, value);
    }
  }
}

static int point_equal(potrace_dpoint_t a, potrace_dpoint_t b) {
  return memcmp(&a, &b, sizeof(potrace_dpoint_t)) == 0;
}

static int state_equal(const potrace_state_t *a, const potrace_state_t *b,
		       const char **reason) {
  const potrace_path_t *pa;
  const potrace_path_t *pb;
  int i;

  if (!a || !b) {
    if (a != b) {
      *reason = "one state is NULL";
      return 0;
    }
    return 1;
  }
  if (a->status != b->status) {
    *reason = "state status differs";
    return 0;
  }

  pa = a->plist;
  pb = b->plist;
  while (pa && pb) {
    if (pa->area != pb->area || pa->sign != pb->sign
	|| pa->curve.n != pb->curve.n) {
      *reason = "path header differs";
      return 0;
    }
    for (i = 0; i < pa->curve.n; i++) {
      if (pa->curve.tag[i] != pb->curve.tag[i]) {
	*reason = "segment tag differs";
	return 0;
      }
      if (pa->curve.tag[i] == POTRACE_CORNER) {
	if (!point_equal(pa->curve.c[i][1], pb->curve.c[i][1])
	    || !point_equal(pa->curve.c[i][2], pb->curve.c[i][2])) {
	  *reason = "corner control point differs";
	  return 0;
	}
      } else {
	if (!point_equal(pa->curve.c[i][0], pb->curve.c[i][0])
	    || !point_equal(pa->curve.c[i][1], pb->curve.c[i][1])
	    || !point_equal(pa->curve.c[i][2], pb->curve.c[i][2])) {
	  *reason = "curve control point differs";
	  return 0;
	}
      }
    }
    pa = pa->next;
    pb = pb->next;
  }
  if (pa || pb) {
    *reason = "path count differs";
    return 0;
  }
  *reason = "exact";
  return 1;
}

static int path_count(const potrace_path_t *p) {
  int n = 0;

  while (p) {
    n++;
    p = p->next;
  }
  return n;
}

static void sign_string(const potrace_path_t *p, char *buf, size_t size) {
  size_t i = 0;

  while (p && i + 1 < size) {
    buf[i++] = (char)p->sign;
    p = p->next;
  }
  buf[i] = '\0';
  if (i == 0) {
    strcpy(buf, "-");
  }
}

static int fail(const char *message, size_t index) {
  fprintf(stderr, "FAIL: item %lu: %s\n", (unsigned long)index, message);
  return 1;
}

int main(void) {
  potrace_param_t *param;
  potrace_param_t *default_param;
  potrace_batch_param_t *bp;
  potrace_batch_result_t *batch;
  potrace_state_t *direct[8];
  potrace_bitmap_t *bitmaps[8];
  potrace_bitmap_t *single;
  potrace_bitmap_t *ring;
  potrace_bitmap_t *bad_map;
  potrace_bitmap_t *many[100];
  const char *names[8] = {
    "empty-0x0", "empty-7x0", "blank-4x4", "single-1x1",
    "ring-hole-9x9", "NULL-bitmap", "bad-map-4x4", "single-5x4"
  };
  const char *reason;
  int expected_paths[8] = {0, 0, 0, 1, 2, -1, -1, 1};
  int expected_status[8] = {
    POTRACE_STATUS_OK, POTRACE_STATUS_OK, POTRACE_STATUS_OK, POTRACE_STATUS_OK,
    POTRACE_STATUS_OK, POTRACE_BATCH_STATUS_INVALID,
    POTRACE_BATCH_STATUS_INVALID, POTRACE_STATUS_OK
  };
  int failures = 0;
  size_t i;
  int invalid_arg;

  param = potrace_param_default();
  default_param = potrace_param_default();
  bp = potrace_batch_param_default();
  if (!param || !default_param || !bp) {
    fprintf(stderr, "FAIL: cannot allocate parameters\n");
    return 1;
  }
  param->turdsize = 0;
  bp->threads = 8;
  bp->chunk_size = 3;
  bp->memory_limit = POTRACE_BATCH_DEFAULT_MEMORY_LIMIT;

  bitmaps[0] = bitmap_new(0, 0);
  bitmaps[1] = bitmap_new(7, 0);
  bitmaps[2] = bitmap_new(4, 4);
  bitmaps[3] = bitmap_new(1, 1);
  bitmaps[4] = bitmap_new(9, 9);
  bitmaps[5] = NULL;
  bitmaps[6] = bitmap_new(4, 4);
  bitmaps[7] = bitmap_new(5, 4);
  if (!bitmaps[0] || !bitmaps[1] || !bitmaps[2] || !bitmaps[3]
      || !bitmaps[4] || !bitmaps[6] || !bitmaps[7]) {
    fprintf(stderr, "FAIL: cannot allocate test bitmaps\n");
    return 1;
  }

  bitmap_put(bitmaps[3], 0, 0, 1);
  fill_rect(bitmaps[4], 0, 0, 9, 9, 1);
  fill_rect(bitmaps[4], 3, 3, 6, 6, 0);
  bitmap_put(bitmaps[7], 3, 2, 1);
  bad_map = bitmaps[6];
  free(bad_map->map);
  bad_map->map = NULL;

  for (i = 0; i < 8; i++) {
    direct[i] = NULL;
  }

  batch = potrace_batch_trace(param, (const potrace_bitmap_t *const *)bitmaps,
			      8, bp);
  if (!batch) {
    fprintf(stderr, "FAIL: batch call failed: %s\n", strerror(errno));
    return 1;
  }
  if (batch->count != 8 || !batch->items) {
    fprintf(stderr, "FAIL: wrong batch shape\n");
    return 1;
  }

  printf("%-16s %-10s %5s %-7s %-12s %s\n",
	 "case", "status", "paths", "signs", "direct", "compare");
  for (i = 0; i < 8; i++) {
    potrace_batch_item_t *item = &batch->items[i];
    char signs[32];
    int paths;

    if (item->status != expected_status[i]) {
      failures += fail("unexpected item status", i);
      continue;
    }
    if (item->status == POTRACE_BATCH_STATUS_INVALID) {
      if (item->state != NULL || item->errnum != EINVAL) {
	failures += fail("invalid item has wrong state/errno", i);
      }
      printf("%-16s %-10s %5s %-7s %-12s %s\n",
	     names[i], "INVALID", "-", "-", "-", "clear error");
      continue;
    }

    direct[i] = potrace_trace(param, bitmaps[i]);
    if (!direct[i]) {
      failures += fail("direct trace failed", i);
      continue;
    }
    paths = path_count(item->state ? item->state->plist : NULL);
    if (paths != expected_paths[i]) {
      failures += fail("unexpected path count", i);
    }
    if (item->state && !state_equal(direct[i], item->state, &reason)) {
      failures += fail(reason, i);
    }
    sign_string(item->state ? item->state->plist : NULL, signs, sizeof(signs));
    printf("%-16s %-10s %5d %-7s %-12s %s\n",
	   names[i],
	   item->status == POTRACE_STATUS_OK ? "OK" : "INCOMPLETE",
	   paths, signs, "OK", "exact");
  }

#ifdef HAVE_PTHREAD
  if (batch->threads_used != 8) {
#else
  if (batch->threads_used != 1) {
#endif
    failures += fail("unexpected worker count", 0);
  }
  if (batch->total_pixels != 16 + 1 + 81 + 20) {
    failures += fail("unexpected total_pixels", 0);
  }

  potrace_batch_result_free(batch);
  for (i = 0; i < 8; i++) {
    potrace_state_free(direct[i]);
  }

  /* A single pixel is legal. With the default turdsize it is suppressed. */
  single = bitmap_new(1, 1);
  bitmap_put(single, 0, 0, 1);
  bp->threads = 8;
  batch = potrace_batch_trace(default_param,
			      (const potrace_bitmap_t *const *)&single, 1, bp);
  if (!batch) {
    failures += fail("default-turdsize batch failed", 0);
  } else {
    if (batch->items[0].status != POTRACE_STATUS_OK
	|| batch->items[0].state == NULL
	|| batch->items[0].state->plist != NULL) {
      failures += fail("default turdsize should suppress one-pixel path", 0);
    }
  }
  potrace_batch_result_free(batch);

  /* Empty batch is valid even without a bitmap array. */
  batch = potrace_batch_trace(param, NULL, 0, bp);
  if (!batch || batch->count != 0 || batch->items != NULL
      || batch->threads_used != 0) {
    failures += fail("empty batch shape is wrong", 0);
  }
  potrace_batch_result_free(batch);

  /* NULL input array with a non-zero count is an argument error. */
  errno = 0;
  batch = potrace_batch_trace(param, NULL, 1, bp);
  invalid_arg = batch == NULL && errno == EINVAL;
  if (!invalid_arg) {
    failures += fail("NULL array with count=1 was not rejected", 0);
  }
  potrace_batch_result_free(batch);

  /* The cap is honored before tracing: every valid item reports ENOMEM. */
  ring = bitmaps[4];
  bp->memory_limit = 1;
  batch = potrace_batch_trace(param, (const potrace_bitmap_t *const *)&ring,
			      1, bp);
  if (!batch) {
    failures += fail("low-memory batch failed", 0);
  } else {
    if (batch->items[0].status != POTRACE_BATCH_STATUS_NO_MEMORY
	|| batch->items[0].state != NULL
	|| batch->items[0].errnum != ENOMEM) {
      failures += fail("low-memory item is not clearly reported", 0);
    }
  }
  potrace_batch_result_free(batch);

  /* A cap that fits the input arena but not all outputs rejects only the
     items that cannot be retained; earlier and later items still run. */
  for (i = 0; i < 100; i++) {
    many[i] = bitmaps[4];
  }
  bp->threads = 4;
  bp->chunk_size = 8;
  bp->memory_limit = 16 * 1024;
  batch = potrace_batch_trace(param, (const potrace_bitmap_t *const *)many,
			      100, bp);
  if (!batch) {
    failures += fail("state-budget batch failed", 0);
  } else {
    int ok = 0;
    int no_memory = 0;
    int bad = 0;

    for (i = 0; i < 100; i++) {
      if (batch->items[i].status == POTRACE_STATUS_OK) {
	ok++;
      } else if (batch->items[i].status == POTRACE_BATCH_STATUS_NO_MEMORY
		 && batch->items[i].state == NULL
		 && batch->items[i].errnum == ENOMEM) {
	no_memory++;
      } else {
	bad++;
      }
    }
    if (ok == 0 || no_memory == 0 || bad != 0) {
      failures += fail("state-budget failures are not isolated", 0);
    }
  }
  potrace_batch_result_free(batch);

  bitmap_free(bitmaps[0]);
  bitmap_free(bitmaps[1]);
  bitmap_free(bitmaps[2]);
  bitmap_free(bitmaps[3]);
  bitmap_free(bitmaps[4]);
  bitmap_free(bitmaps[6]);
  bitmap_free(bitmaps[7]);
  bitmap_free(single);
  potrace_param_free(param);
  potrace_param_free(default_param);
  potrace_batch_param_free(bp);

  if (failures) {
    fprintf(stderr, "batch-check: %d failure(s)\n", failures);
    return 1;
  }
  printf("batch-check: all boundary and consistency checks passed\n");
  return 0;
}
