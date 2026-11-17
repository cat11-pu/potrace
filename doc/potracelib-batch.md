# Potrace batch tracing API

`potrace_batch_trace()` traces an array of bitmaps while keeping the
per-image `potrace_trace()` result unchanged. Results are returned in
input order, and one bad bitmap does not stop the rest of the batch.

## Interface

```c
potrace_batch_param_t *potrace_batch_param_default(void);
void potrace_batch_param_free(potrace_batch_param_t *p);

potrace_batch_result_t *potrace_batch_trace(
    const potrace_param_t *param,
    const potrace_bitmap_t *const *bitmaps,
    size_t count,
    const potrace_batch_param_t *batch_param);

void potrace_batch_result_free(potrace_batch_result_t *result);
```

`potrace_batch_param_t` has three fields:

| field | meaning | default |
|---|---|---|
| `threads` | worker count; `0` selects automatically, capped at 8 | `0` |
| `memory_limit` | batch-owned byte cap; `0` means 512 MiB | 512 MiB |
| `chunk_size` | images per work chunk; `0` selects automatically | `0` |

The request count may be zero. `bitmaps` may then be `NULL`. A non-zero
count requires a non-NULL array. Every array entry may independently be
NULL or invalid; that item is reported as `POTRACE_BATCH_STATUS_INVALID`
with `errnum == EINVAL`.

The result layout is:

```c
struct potrace_batch_result_s {
  size_t count;
  potrace_batch_item_t *items;   /* exactly one per input */
  size_t total_pixels;           /* sum of valid w*h */
  size_t preallocated_bytes;     /* input arena + bookkeeping */
  int threads_used;              /* actual workers */
};

struct potrace_batch_item_s {
  int status;                    /* OK, INCOMPLETE, or an error code */
  int errnum;                    /* meaningful when state == NULL */
  potrace_state_t *state;        /* NULL on item failure */
};
```

`POTRACE_STATUS_INCOMPLETE` is passed through unchanged. The batch error
codes are:

| status | meaning |
|---|---|
| `POTRACE_BATCH_STATUS_INVALID` (`-1`) | malformed bitmap descriptor |
| `POTRACE_BATCH_STATUS_FAILED` (`-2`) | `potrace_trace()` failed |
| `POTRACE_BATCH_STATUS_NO_MEMORY` (`-3`) | item does not fit the cap |

## Memory and execution

Before tracing, the batch validates every bitmap, computes
`ceil(w / word_bits) * h` words for each valid bitmap, and allocates one
arena for the sum. Input bitmaps are copied into that arena and are not
modified. Work is divided into chunks and claimed by the worker pool.
Each chunk writes disjoint result slots, so output order is deterministic.

The cap includes the preallocated input arena, batch bookkeeping, and
retained `potrace_state_t` result trees. If the input arena alone does not
fit, all valid items report `NO_MEMORY`; the result object is still
returned. If a completed state does not fit the remaining budget, only
that item is released and marked `NO_MEMORY`; other items continue. Call
`potrace_batch_result_free()` to release all retained states.

The progress callback in `param->progress` is disabled for worker calls,
because a single callback cannot safely represent concurrent per-image
progress. All tracing parameters other than progress are honored.

If POSIX threads are unavailable or `--disable-threads` is used, the
same API runs with one worker. On POSIX builds, `threads` is capped at 64.

## Boundary and hole cases

The automated `check/batch-check` test uses `turdsize = 0` for the table
below, except where noted.

| input | item status | output |
|---|---|---|
| `0 x 0` | `OK` | `plist == NULL`, zero paths |
| `7 x 0` | `OK` | `plist == NULL`, zero paths |
| blank `4 x 4` | `OK` | `plist == NULL`, zero paths |
| one black `1 x 1` pixel | `OK` | one positive path, area 1 |
| `9 x 9` square with `3 x 3` hole | `OK` | two paths, signs `+ -` |
| one black pixel with default `turdsize = 2` | `OK` | suppressed, zero paths |
| `NULL` bitmap entry | `INVALID` | `state == NULL`, `errno == EINVAL` |
| `4 x 4`, `map == NULL` | `INVALID` | `state == NULL`, `errno == EINVAL` |
| cap smaller than the input arena | `NO_MEMORY` | `state == NULL`, `errno == ENOMEM` |

Zero width or zero height is a valid empty bitmap. Negative dimensions,
a missing map for a non-empty bitmap, or a scanline stride smaller than
the row width are invalid.

## Batch consistency

`check/batch-check` calls `potrace_trace()` once per valid input and
compares the batch result path-by-path and control-point-by-control-point.
The comparison covers path order, `area`, `sign`, `curve.n`, every segment
tag, and every meaningful `curve.c` coordinate. On the current test set,
all six valid mixed-batch items match exactly.

`check/batch-bench` generates 1000 deterministic 512 x 512 icons and
compares the same serialized path data. A representative local run:

| metric | result |
|---|---|
| workers | 8 |
| sequential per-image calls | 0.61 s |
| batch call | 0.25 s |
| time reduction | 58.5% |
| speedup | 2.41x |
| exact result matches | 1000 / 1000 |
| peak RSS | about 225 MiB |
| configured cap | 512 MiB |

Run it with:

```sh
./configure --with-libpotrace
make
make -C check batch-check batch-bench
./check/batch-check
./check/batch-bench 1000 8 16
```
