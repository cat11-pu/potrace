/* Copyright (C) 2001-2019 Peter Selinger.
   This file is part of Potrace. It is free software and it is covered
   by the GNU General Public License. See the file COPYING for details. */

#ifndef POTRACELIB_H
#define POTRACELIB_H

#include <stddef.h>

/* this file defines the API for the core Potrace library. For a more
   detailed description of the API, see potracelib.pdf */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------- */
/* tracing parameters */

/* turn policies */
#define POTRACE_TURNPOLICY_BLACK 0
#define POTRACE_TURNPOLICY_WHITE 1
#define POTRACE_TURNPOLICY_LEFT 2
#define POTRACE_TURNPOLICY_RIGHT 3
#define POTRACE_TURNPOLICY_MINORITY 4
#define POTRACE_TURNPOLICY_MAJORITY 5
#define POTRACE_TURNPOLICY_RANDOM 6

/* structure to hold progress bar callback data */
struct potrace_progress_s {
  void (*callback)(double progress, void *privdata); /* callback fn */
  void *data;          /* callback function's private data */
  double min, max;     /* desired range of progress, e.g. 0.0 to 1.0 */
  double epsilon;      /* granularity: can skip smaller increments */
};
typedef struct potrace_progress_s potrace_progress_t;

/* structure to hold tracing parameters */
struct potrace_param_s {
  int turdsize;        /* area of largest path to be ignored */
  int turnpolicy;      /* resolves ambiguous turns in path decomposition */
  double alphamax;     /* corner threshold */
  int opticurve;       /* use curve optimization? */
  double opttolerance; /* curve optimization tolerance */
  potrace_progress_t progress; /* progress callback function */
};
typedef struct potrace_param_s potrace_param_t;

/* ---------------------------------------------------------------------- */
/* bitmaps */

/* native word size */
typedef unsigned long potrace_word;

/* Internal bitmap format. The n-th scanline starts at scanline(n) =
   (map + n*dy). Raster data is stored as a sequence of potrace_words
   (NOT bytes). The leftmost bit of scanline n is the most significant
   bit of scanline(n)[0]. */
struct potrace_bitmap_s {
  int w, h;              /* width and height, in pixels */
  int dy;                /* words per scanline (not bytes) */
  potrace_word *map;     /* raw data, dy*h words */
};
typedef struct potrace_bitmap_s potrace_bitmap_t;

/* ---------------------------------------------------------------------- */
/* curves */

/* point */
struct potrace_dpoint_s {
  double x, y;
};
typedef struct potrace_dpoint_s potrace_dpoint_t;

/* segment tags */
#define POTRACE_CURVETO 1
#define POTRACE_CORNER 2

/* closed curve segment */
struct potrace_curve_s {
  int n;                    /* number of segments */
  int *tag;                 /* tag[n]: POTRACE_CURVETO or POTRACE_CORNER */
  potrace_dpoint_t (*c)[3]; /* c[n][3]: control points. 
			       c[n][0] is unused for tag[n]=POTRACE_CORNER */
};
typedef struct potrace_curve_s potrace_curve_t;

/* Linked list of signed curve segments. Also carries a tree structure. */
struct potrace_path_s {
  int area;                         /* area of the bitmap path */
  int sign;                         /* '+' or '-', depending on orientation */
  potrace_curve_t curve;            /* this path's vector data */

  struct potrace_path_s *next;      /* linked list structure */

  struct potrace_path_s *childlist; /* tree structure */
  struct potrace_path_s *sibling;   /* tree structure */

  struct potrace_privpath_s *priv;  /* private state */
};
typedef struct potrace_path_s potrace_path_t;  

/* ---------------------------------------------------------------------- */
/* Potrace state */

#define POTRACE_STATUS_OK         0
#define POTRACE_STATUS_INCOMPLETE 1

struct potrace_state_s {
  int status;                       
  potrace_path_t *plist;            /* vector data */

  struct potrace_privstate_s *priv; /* private state */
};
typedef struct potrace_state_s potrace_state_t;

/* ---------------------------------------------------------------------- */
/* batch tracing */

/* per-item status. POTRACE_STATUS_OK and POTRACE_STATUS_INCOMPLETE are
   also used for successfully traced items. */
#define POTRACE_BATCH_STATUS_INVALID    (-1)
#define POTRACE_BATCH_STATUS_FAILED     (-2)
#define POTRACE_BATCH_STATUS_NO_MEMORY  (-3)

/* defaults used by potrace_batch_param_default(). A threads value of 0
   means "choose automatically, up to 8". A chunk_size value of 0 means
   "choose automatically". */
#define POTRACE_BATCH_DEFAULT_THREADS 8
#define POTRACE_BATCH_DEFAULT_CHUNK_SIZE 0
#define POTRACE_BATCH_DEFAULT_MEMORY_LIMIT ((size_t)512 * 1024 * 1024)

struct potrace_batch_param_s {
  int threads;         /* worker count, or 0 for automatic */
  size_t memory_limit; /* batch-owned bytes, or 0 for the default */
  size_t chunk_size;   /* images per work chunk, or 0 for automatic */
};
typedef struct potrace_batch_param_s potrace_batch_param_t;

struct potrace_batch_item_s {
  int status;                /* POTRACE_STATUS_* or POTRACE_BATCH_STATUS_* */
  int errnum;                /* error number for failed items, else 0 */
  potrace_state_t *state;    /* result, or NULL if the item failed */
};
typedef struct potrace_batch_item_s potrace_batch_item_t;

struct potrace_batch_result_s {
  size_t count;              /* number of items, equal to the input count */
  potrace_batch_item_t *items; /* indexed exactly like the input array */
  size_t total_pixels;       /* sum of w*h over valid input bitmaps */
  size_t preallocated_bytes; /* input arena + batch bookkeeping */
  int threads_used;          /* actual number of workers */
};
typedef struct potrace_batch_result_s potrace_batch_result_t;

/* ---------------------------------------------------------------------- */
/* API functions */

/* get default parameters */
potrace_param_t *potrace_param_default(void);

/* free parameter set */
void potrace_param_free(potrace_param_t *p);

/* trace a bitmap */
potrace_state_t *potrace_trace(const potrace_param_t *param, 
			       const potrace_bitmap_t *bm);

/* get default batch parameters */
potrace_batch_param_t *potrace_batch_param_default(void);

/* free a batch parameter set */
void potrace_batch_param_free(potrace_batch_param_t *p);

/* Trace a batch of bitmaps. Bitmaps are validated and copied into one
   arena sized from the sum of their pixel counts, then traced by a
   pool of worker threads in chunks. The result contains one item per
   input, in the same order, regardless of individual failures. The
   input bitmaps remain owned by the caller. Returns NULL with errno
   set only for invalid arguments or failure to allocate the result
   itself; per-item failures are reported in the item status. The
   progress callback in param is not invoked for batch calls. */
potrace_batch_result_t *potrace_batch_trace(
  const potrace_param_t *param,
  const potrace_bitmap_t *const *bitmaps,
  size_t count,
  const potrace_batch_param_t *batch_param);

/* free a batch result and all states it contains */
void potrace_batch_result_free(potrace_batch_result_t *result);

/* free a Potrace state */
void potrace_state_free(potrace_state_t *st);

/* return a static plain text version string identifying this version
   of potracelib */
const char *potrace_version(void);

#ifdef  __cplusplus
} /* end of extern "C" */
#endif

#endif /* POTRACELIB_H */
