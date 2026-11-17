#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/bitmap.h"
#include "../src/potracelib.h"

#define DEFAULT_CASES 200
#define DEFAULT_SEED UINT64_C(0x9e3779b97f4a7c15)
#define CHILD_TIMEOUT 30
#define MAX_DIAG 512
#define MAX_STDERR_READ 16384

#define ENCODE_MAGIC UINT32_C(0x50545243)
#define ENCODE_OK 0
#define ENCODE_TRACE_ERROR 1
#define ENCODE_NONFINITE 2
#define ENCODE_INVALID 3

typedef enum {
  FAMILY_HOLES,
  FAMILY_PIXEL,
  FAMILY_COLLINEAR,
  FAMILY_OVERSIZED,
  FAMILY_COUNT
} family_t;

typedef enum {
  OUTCOME_OK,
  OUTCOME_CRASH,
  OUTCOME_TIMEOUT,
  OUTCOME_NONFINITE,
  OUTCOME_NONDETERMINISTIC,
  OUTCOME_TRACE_ERROR,
  OUTCOME_RUNTIME_ERROR,
  OUTCOME_HARNESS_ERROR,
  OUTCOME_COUNT
} outcome_t;

typedef struct {
  uint64_t state;
} rng_t;

typedef struct {
  int turdsize;
  int turnpolicy;
  double alphamax;
  int opticurve;
  double opttolerance;
} trace_params_t;

typedef struct {
  int w;
  int h;
  unsigned char *pixels;
  int index;
  family_t family;
  trace_params_t params;
} image_t;

typedef struct {
  unsigned char *data;
  size_t len;
  size_t cap;
  int error;
} buffer_t;

typedef struct {
  outcome_t outcome;
  int term_signal;
  int exit_code;
  unsigned char *encoded;
  size_t encoded_len;
  char diag[MAX_DIAG];
  int harness_error;
} run_result_t;

typedef struct {
  outcome_t outcome;
  char diag[MAX_DIAG];
} evaluation_t;

typedef struct {
  int index;
  int original_w;
  int original_h;
  int original_pixels;
  int minimal_w;
  int minimal_h;
  int minimal_pixels;
  family_t family;
  outcome_t outcome;
  trace_params_t params;
  image_t minimal;
  char diag[MAX_DIAG];
} counterexample_t;

static const char *family_name(family_t family) {
  switch (family) {
  case FAMILY_HOLES:
    return "holes";
  case FAMILY_PIXEL:
    return "pixel";
  case FAMILY_COLLINEAR:
    return "collinear";
  case FAMILY_OVERSIZED:
    return "oversized";
  default:
    return "unknown";
  }
}

static const char *outcome_name(outcome_t outcome) {
  switch (outcome) {
  case OUTCOME_OK:
    return "ok";
  case OUTCOME_CRASH:
    return "crash";
  case OUTCOME_TIMEOUT:
    return "timeout";
  case OUTCOME_NONFINITE:
    return "nonfinite";
  case OUTCOME_NONDETERMINISTIC:
    return "nondeterministic";
  case OUTCOME_TRACE_ERROR:
    return "trace-error";
  case OUTCOME_RUNTIME_ERROR:
    return "runtime-error";
  case OUTCOME_HARNESS_ERROR:
    return "harness-error";
  default:
    return "unknown";
  }
}

static void rng_init(rng_t *rng, uint64_t seed) {
  rng->state = seed;
}

static uint64_t rng_next(rng_t *rng) {
  uint64_t z;

  rng->state += UINT64_C(0x9e3779b97f4a7c15);
  z = rng->state;
  z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
  return z ^ (z >> 31);
}

static int rng_below(rng_t *rng, int bound) {
  if (bound <= 1) {
    return 0;
  }
  return (int)(rng_next(rng) % (uint64_t)bound);
}

static int imin(int a, int b) {
  return a < b ? a : b;
}

static int imax(int a, int b) {
  return a > b ? a : b;
}

static int image_init(image_t *img, int w, int h, family_t family, int index) {
  if (w <= 0 || h <= 0 || (size_t)w > SIZE_MAX / (size_t)h) {
    return 0;
  }
  img->w = w;
  img->h = h;
  img->pixels = (unsigned char *)calloc((size_t)w * (size_t)h, 1);
  if (img->pixels == NULL) {
    return 0;
  }
  img->family = family;
  img->index = index;
  return 1;
}

static void image_free(image_t *img) {
  free(img->pixels);
  memset(img, 0, sizeof(*img));
}

static int image_clone(image_t *dst, const image_t *src) {
  size_t size;

  *dst = *src;
  dst->pixels = NULL;
  if (src->pixels == NULL) {
    return 1;
  }
  size = (size_t)src->w * (size_t)src->h;
  dst->pixels = (unsigned char *)malloc(size);
  if (dst->pixels == NULL) {
    return 0;
  }
  memcpy(dst->pixels, src->pixels, size);
  return 1;
}

static int image_get(const image_t *img, int x, int y) {
  if (x < 0 || y < 0 || x >= img->w || y >= img->h) {
    return 0;
  }
  return img->pixels[(size_t)y * (size_t)img->w + (size_t)x] != 0;
}

static void image_set(image_t *img, int x, int y, int value) {
  if (x < 0 || y < 0 || x >= img->w || y >= img->h) {
    return;
  }
  img->pixels[(size_t)y * (size_t)img->w + (size_t)x] = value ? 1 : 0;
}

static void image_fill(image_t *img, int x0, int y0, int x1, int y1, int value) {
  int x;
  int y;

  if (x0 > x1) {
    int tmp = x0;
    x0 = x1;
    x1 = tmp;
  }
  if (y0 > y1) {
    int tmp = y0;
    y0 = y1;
    y1 = tmp;
  }
  x0 = imax(x0, 0);
  y0 = imax(y0, 0);
  x1 = imin(x1, img->w - 1);
  y1 = imin(y1, img->h - 1);
  for (y = y0; y <= y1; y++) {
    for (x = x0; x <= x1; x++) {
      image_set(img, x, y, value);
    }
  }
}

static int image_count(const image_t *img) {
  size_t i;
  size_t size = (size_t)img->w * (size_t)img->h;
  int count = 0;

  for (i = 0; i < size; i++) {
    count += img->pixels[i] != 0;
  }
  return count;
}

static int image_has_neighbor(const image_t *img, int x, int y) {
  int dx;
  int dy;

  for (dy = -1; dy <= 1; dy++) {
    for (dx = -1; dx <= 1; dx++) {
      if ((dx != 0 || dy != 0) && image_get(img, x + dx, y + dy)) {
        return 1;
      }
    }
  }
  return 0;
}

static void generate_params(rng_t *rng, trace_params_t *params) {
  static const double alphas[] = {0.0, 0.5, 1.0, 1.3334, 2.0};
  static const double tolerances[] = {0.0, 0.2, 1.0, 5.0};

  params->turdsize = rng_below(rng, 101);
  params->turnpolicy = rng_below(rng, 7);
  params->alphamax = alphas[rng_below(rng, (int)(sizeof(alphas) / sizeof(alphas[0])))];
  params->opticurve = rng_below(rng, 2);
  params->opttolerance =
      tolerances[rng_below(rng, (int)(sizeof(tolerances) / sizeof(tolerances[0])))];
}

static void generate_holes(image_t *img, rng_t *rng) {
  int w = 19 + rng_below(rng, 92);
  int h = 19 + rng_below(rng, 92);
  int blocks;
  int i;

  image_init(img, w, h, FAMILY_HOLES, img->index);
  blocks = 1 + rng_below(rng, 5);
  for (i = 0; i < blocks; i++) {
    int bw = 5 + rng_below(rng, imax(1, w / 2));
    int bh = 5 + rng_below(rng, imax(1, h / 2));
    int x0 = rng_below(rng, imax(1, w - bw));
    int y0 = rng_below(rng, imax(1, h - bh));
    int x1 = imin(w - 1, x0 + bw - 1);
    int y1 = imin(h - 1, y0 + bh - 1);
    int margin = 1 + rng_below(rng, 2);

    image_fill(img, x0, y0, x1, y1, 1);
    if (x1 - x0 >= 2 * margin + 1 && y1 - y0 >= 2 * margin + 1) {
      image_fill(img, x0 + margin, y0 + margin, x1 - margin, y1 - margin, 0);
    }
  }
  for (i = 0; i < w * h / 200; i++) {
    image_set(img, rng_below(rng, w), rng_below(rng, h), 1);
  }
  image_fill(img, 2, 2, imin(8, w - 2), imin(8, h - 2), 1);
  image_set(img, imin(5, w - 3), imin(5, h - 3), 0);
}

static void generate_pixels(image_t *img, rng_t *rng) {
  int w = 5 + rng_below(rng, 124);
  int h = 5 + rng_below(rng, 124);
  int target = 1 + rng_below(rng, 160);
  int attempts;
  int count = 0;

  image_init(img, w, h, FAMILY_PIXEL, img->index);
  for (attempts = 0; attempts < target * 20 && count < target; attempts++) {
    int x = rng_below(rng, w);
    int y = rng_below(rng, h);

    if (!image_has_neighbor(img, x, y)) {
      image_set(img, x, y, 1);
      count++;
    }
  }
  if (count == 0) {
    image_set(img, w / 2, h / 2, 1);
  }
}

static void generate_collinear(image_t *img, rng_t *rng) {
  int w = 17 + rng_below(rng, 160);
  int h = 17 + rng_below(rng, 160);
  int i;
  int base_y = 2 + rng_below(rng, imax(1, h - 5));
  int base_x = 2 + rng_below(rng, imax(1, w - 5));

  image_init(img, w, h, FAMILY_COLLINEAR, img->index);
  for (i = 0; i < 4; i++) {
    int x0 = rng_below(rng, imax(1, w / 2));
    int x1 = x0 + 3 + rng_below(rng, imax(1, w - x0));
    image_fill(img, x0, base_y, x1, base_y, 1);
  }
  for (i = 0; i < 4; i++) {
    int y0 = rng_below(rng, imax(1, h / 2));
    int y1 = y0 + 3 + rng_below(rng, imax(1, h - y0));
    image_fill(img, base_x, y0, base_x, y1, 1);
  }
  for (i = 0; i < 4; i++) {
    int x0 = rng_below(rng, imax(1, w));
    int y0 = rng_below(rng, imax(1, h));
    int len = 4 + rng_below(rng, imax(1, w / 2));
    int thick = 1 + rng_below(rng, 3);
    if (i & 1) {
      image_fill(img, x0, y0, x0 + len, y0 + thick - 1, 1);
    } else {
      image_fill(img, x0, y0, x0 + thick - 1, y0 + len, 1);
    }
  }
}

static void generate_oversized(image_t *img, rng_t *rng) {
  int w;
  int h;
  int i;

  switch (img->index % 3) {
  case 0:
    w = 4096 + rng_below(rng, 12289);
    h = 24 + rng_below(rng, 232);
    break;
  case 1:
    w = 24 + rng_below(rng, 232);
    h = 4096 + rng_below(rng, 12289);
    break;
  default:
    w = 1024 + rng_below(rng, 3072);
    h = 1024 + rng_below(rng, 3072);
    break;
  }

  image_init(img, w, h, FAMILY_OVERSIZED, img->index);
  for (i = 0; i < 24; i++) {
    int x0 = rng_below(rng, w);
    int y0 = rng_below(rng, h);
    int len = 1 + rng_below(rng, imin(imax(w, h), 4096));
    if (i & 1) {
      image_fill(img, x0, y0, x0 + len, y0, 1);
    } else {
      image_fill(img, x0, y0, x0, y0 + len, 1);
    }
  }
  for (i = 0; i < 12; i++) {
    int x = rng_below(rng, imax(1, w - 2));
    int y = rng_below(rng, imax(1, h - 2));
    switch (i % 4) {
    case 0:
      image_set(img, x, y, 1);
      image_set(img, x + 1, y + 1, 1);
      break;
    case 1:
      image_set(img, x, y, 1);
      image_set(img, x + 1, y, 1);
      image_set(img, x, y + 1, 1);
      break;
    case 2:
      image_fill(img, x, y, x + 2, y, 1);
      break;
    default:
      image_fill(img, x, y, x, y + 2, 1);
      break;
    }
  }
}

static int generate_case(int index, uint64_t seed, image_t *img) {
  uint64_t case_seed = seed ^ (UINT64_C(0xd1b54a32d192ed03) * (uint64_t)(index + 1));
  rng_t rng;

  rng_init(&rng, case_seed);
  memset(img, 0, sizeof(*img));
  img->index = index;
  img->family = (family_t)(index % FAMILY_COUNT);
  generate_params(&rng, &img->params);

  switch (img->family) {
  case FAMILY_HOLES:
    generate_holes(img, &rng);
    break;
  case FAMILY_PIXEL:
    generate_pixels(img, &rng);
    break;
  case FAMILY_COLLINEAR:
    generate_collinear(img, &rng);
    break;
  case FAMILY_OVERSIZED:
    generate_oversized(img, &rng);
    break;
  default:
    return 0;
  }
  return img->pixels != NULL;
}

static int buffer_reserve(buffer_t *buffer, size_t extra) {
  size_t needed;
  size_t capacity;
  unsigned char *next;

  if (buffer->error || extra > SIZE_MAX - buffer->len) {
    buffer->error = 1;
    return 0;
  }
  needed = buffer->len + extra;
  if (needed <= buffer->cap) {
    return 1;
  }
  capacity = buffer->cap ? buffer->cap : 4096;
  while (capacity < needed) {
    if (capacity > SIZE_MAX / 2) {
      capacity = needed;
      break;
    }
    capacity *= 2;
  }
  next = (unsigned char *)realloc(buffer->data, capacity);
  if (next == NULL) {
    buffer->error = 1;
    return 0;
  }
  buffer->data = next;
  buffer->cap = capacity;
  return 1;
}

static void buffer_u32(buffer_t *buffer, uint32_t value) {
  if (buffer_reserve(buffer, sizeof(value))) {
    memcpy(buffer->data + buffer->len, &value, sizeof(value));
    buffer->len += sizeof(value);
  }
}

static void buffer_i32(buffer_t *buffer, int32_t value) {
  if (buffer_reserve(buffer, sizeof(value))) {
    memcpy(buffer->data + buffer->len, &value, sizeof(value));
    buffer->len += sizeof(value);
  }
}

static void buffer_u64(buffer_t *buffer, uint64_t value) {
  if (buffer_reserve(buffer, sizeof(value))) {
    memcpy(buffer->data + buffer->len, &value, sizeof(value));
    buffer->len += sizeof(value);
  }
}

static uint64_t double_bits(double value) {
  uint64_t bits;

  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

static int write_all(int fd, const unsigned char *data, size_t len) {
  while (len > 0) {
    ssize_t written = write(fd, data, len);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return 0;
    }
    data += (size_t)written;
    len -= (size_t)written;
  }
  return 1;
}

static int curve_points_per_segment(int tag) {
  if (tag == POTRACE_CORNER) {
    return 1;
  }
  if (tag == POTRACE_CURVETO) {
    return 3;
  }
  return -1;
}

static int validate_paths(const potrace_path_t *plist) {
  const potrace_path_t *path;

  for (path = plist; path != NULL; path = path->next) {
    int k;
    int n = path->curve.n;

    if (n < 0 || (n > 0 && (path->curve.tag == NULL || path->curve.c == NULL))) {
      return ENCODE_INVALID;
    }
    for (k = 0; k < n; k++) {
      int points = curve_points_per_segment(path->curve.tag[k]);
      int j;

      if (points < 0) {
        return ENCODE_INVALID;
      }
      for (j = 0; j < points; j++) {
        int point_index = points == 1 ? 1 : j;
        double x = path->curve.c[k][point_index].x;
        double y = path->curve.c[k][point_index].y;

        if (!isfinite(x) || !isfinite(y)) {
          return ENCODE_NONFINITE;
        }
      }
    }
  }
  return ENCODE_OK;
}

static int encode_paths(const potrace_state_t *state, buffer_t *buffer) {
  const potrace_path_t *path;
  int npaths = 0;
  int status;

  if (state == NULL) {
    buffer_u32(buffer, ENCODE_MAGIC);
    buffer_i32(buffer, ENCODE_TRACE_ERROR);
    buffer_i32(buffer, 0);
    return buffer->error ? -1 : 0;
  }

  status = validate_paths(state->plist);
  for (path = state->plist; path != NULL; path = path->next) {
    npaths++;
  }

  buffer_u32(buffer, ENCODE_MAGIC);
  buffer_i32(buffer, status);
  buffer_i32(buffer, npaths);
  if (status != ENCODE_OK || buffer->error) {
    return buffer->error ? -1 : 0;
  }

  for (path = state->plist; path != NULL; path = path->next) {
    int k;
    int n = path->curve.n;

    buffer_i32(buffer, path->area);
    buffer_i32(buffer, path->sign);
    buffer_i32(buffer, n);
    for (k = 0; k < n; k++) {
      int points = curve_points_per_segment(path->curve.tag[k]);
      int j;

      buffer_i32(buffer, path->curve.tag[k]);
      buffer_i32(buffer, points);
      for (j = 0; j < points; j++) {
        int point_index = points == 1 ? 1 : j;
        buffer_u64(buffer, double_bits(path->curve.c[k][point_index].x));
        buffer_u64(buffer, double_bits(path->curve.c[k][point_index].y));
      }
    }
  }
  return buffer->error ? -1 : 0;
}

static potrace_bitmap_t *make_bitmap(const image_t *img) {
  potrace_bitmap_t *bm;
  int x;
  int y;

  bm = bm_new(img->w, img->h);
  if (bm == NULL) {
    return NULL;
  }
  for (y = 0; y < img->h; y++) {
    for (x = 0; x < img->w; x++) {
      if (image_get(img, x, y)) {
        BM_SET(bm, x, y);
      }
    }
  }
  return bm;
}

static void child_run(const image_t *img, int fd) {
  potrace_bitmap_t *bm;
  potrace_param_t *param;
  potrace_state_t *state = NULL;
  buffer_t buffer;
  int encoded;

  setenv("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1", 1);
  setenv("ASAN_OPTIONS", "halt_on_error=1", 1);
  alarm(CHILD_TIMEOUT);

  memset(&buffer, 0, sizeof(buffer));
  bm = make_bitmap(img);
  param = potrace_param_default();
  if (bm == NULL || param == NULL) {
    buffer_u32(&buffer, ENCODE_MAGIC);
    buffer_i32(&buffer, ENCODE_TRACE_ERROR);
    buffer_i32(&buffer, 0);
    encoded = buffer.error ? -1 : 0;
  } else {
    param->turdsize = img->params.turdsize;
    param->turnpolicy = img->params.turnpolicy;
    param->alphamax = img->params.alphamax;
    param->opticurve = img->params.opticurve;
    param->opttolerance = img->params.opttolerance;
    state = potrace_trace(param, bm);
    if (state == NULL || state->status != POTRACE_STATUS_OK) {
      buffer_u32(&buffer, ENCODE_MAGIC);
      buffer_i32(&buffer, ENCODE_TRACE_ERROR);
      buffer_i32(&buffer, 0);
      encoded = buffer.error ? -1 : 0;
    } else {
      encoded = encode_paths(state, &buffer);
    }
  }

  if (encoded == 0 && !write_all(fd, buffer.data, buffer.len)) {
    encoded = -1;
  }

  free(buffer.data);
  if (state != NULL) {
    potrace_state_free(state);
  }
  if (param != NULL) {
    potrace_param_free(param);
  }
  bm_free(bm);
  _exit(encoded == 0 ? 0 : 2);
}

static void copy_diag(const unsigned char *stderr_data, size_t stderr_len, char *diag) {
  size_t i;
  size_t length = 0;

  diag[0] = '\0';
  for (i = 0; i < stderr_len && length + 1 < MAX_DIAG; i++) {
    unsigned char c = stderr_data[i];
    if (c == '\n' || c == '\r') {
      break;
    }
    diag[length++] = (char)c;
  }
  diag[length] = '\0';
}

static int has_runtime_diag(const unsigned char *data, size_t len) {
  static const char *needles[] = {
      "runtime error:",
      "AddressSanitizer",
      "UndefinedBehaviorSanitizer",
      "LeakSanitizer",
      "Sanitizer"};
  size_t i;

  if (data == NULL) {
    return 0;
  }
  for (i = 0; i < sizeof(needles) / sizeof(needles[0]); i++) {
    size_t needle_len = strlen(needles[i]);
    size_t j;
    if (needle_len > len) {
      continue;
    }
    for (j = 0; j + needle_len <= len; j++) {
      if (memcmp(data + j, needles[i], needle_len) == 0) {
        return 1;
      }
    }
  }
  return 0;
}

static unsigned char *read_fd_all(int fd, size_t *length) {
  unsigned char *data = NULL;
  size_t len = 0;
  size_t cap = 0;

  *length = 0;
  for (;;) {
    size_t want;
    ssize_t got;

    if (cap - len < 4096) {
      size_t next_cap = cap ? cap * 2 : 4096;
      unsigned char *next;
      if (next_cap < cap + 4096) {
        next_cap = cap + 4096;
      }
      next = (unsigned char *)realloc(data, next_cap);
      if (next == NULL) {
        free(data);
        return NULL;
      }
      data = next;
      cap = next_cap;
    }
    want = cap - len;
    got = read(fd, data + len, want);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      free(data);
      return NULL;
    }
    if (got == 0) {
      break;
    }
    len += (size_t)got;
  }
  *length = len;
  return data;
}

static run_result_t run_once(const image_t *img) {
  run_result_t result;
  int out_pipe[2] = {-1, -1};
  char stderr_path[] = "/tmp/potrace-property-XXXXXX";
  int stderr_fd = -1;
  pid_t pid;
  int wait_status = 0;
  unsigned char *stderr_data = NULL;
  size_t stderr_len = 0;

  memset(&result, 0, sizeof(result));
  result.outcome = OUTCOME_HARNESS_ERROR;
  if (pipe(out_pipe) != 0) {
    result.harness_error = 1;
    return result;
  }
  stderr_fd = mkstemp(stderr_path);
  if (stderr_fd < 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    result.harness_error = 1;
    return result;
  }
  unlink(stderr_path);

  pid = fork();
  if (pid < 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(stderr_fd);
    result.harness_error = 1;
    return result;
  }
  if (pid == 0) {
    close(out_pipe[0]);
    dup2(stderr_fd, STDERR_FILENO);
    close(stderr_fd);
    child_run(img, out_pipe[1]);
    _exit(2);
  }

  close(out_pipe[1]);
  result.encoded = read_fd_all(out_pipe[0], &result.encoded_len);
  close(out_pipe[0]);
  if (lseek(stderr_fd, 0, SEEK_SET) >= 0) {
    stderr_data = read_fd_all(stderr_fd, &stderr_len);
  }
  close(stderr_fd);
  copy_diag(stderr_data, stderr_len, result.diag);

  while (waitpid(pid, &wait_status, 0) < 0) {
    if (errno != EINTR) {
      result.harness_error = 1;
      free(stderr_data);
      return result;
    }
  }

  if (WIFSIGNALED(wait_status)) {
    result.term_signal = WTERMSIG(wait_status);
    result.outcome =
        result.term_signal == SIGALRM ? OUTCOME_TIMEOUT : OUTCOME_CRASH;
    free(stderr_data);
    return result;
  }

  if (!WIFEXITED(wait_status)) {
    result.harness_error = 1;
    free(stderr_data);
    return result;
  }

  result.exit_code = WEXITSTATUS(wait_status);
  if (has_runtime_diag(stderr_data, stderr_len)) {
    result.outcome = OUTCOME_RUNTIME_ERROR;
    free(stderr_data);
    return result;
  }
  if (result.exit_code != 0 || result.encoded == NULL ||
      result.encoded_len < 3 * sizeof(uint32_t)) {
    result.harness_error = 1;
    free(stderr_data);
    return result;
  }

  {
    uint32_t magic;
    int32_t encoded_status;
    memcpy(&magic, result.encoded, sizeof(magic));
    memcpy(&encoded_status, result.encoded + sizeof(uint32_t),
           sizeof(encoded_status));
    if (magic != ENCODE_MAGIC) {
      result.harness_error = 1;
      free(stderr_data);
      return result;
    }
    switch (encoded_status) {
    case ENCODE_OK:
      result.outcome = OUTCOME_OK;
      break;
    case ENCODE_NONFINITE:
      result.outcome = OUTCOME_NONFINITE;
      break;
    case ENCODE_INVALID:
      result.outcome = OUTCOME_NONFINITE;
      break;
    case ENCODE_TRACE_ERROR:
      result.outcome = OUTCOME_TRACE_ERROR;
      break;
    default:
      result.outcome = OUTCOME_HARNESS_ERROR;
      result.harness_error = 1;
      break;
    }
  }

  free(stderr_data);
  return result;
}

static void run_result_free(run_result_t *result) {
  free(result->encoded);
  result->encoded = NULL;
  result->encoded_len = 0;
}

static int same_encoded(const run_result_t *a, const run_result_t *b) {
  if (a->encoded_len != b->encoded_len) {
    return 0;
  }
  if (a->encoded_len == 0) {
    return 1;
  }
  return memcmp(a->encoded, b->encoded, a->encoded_len) == 0;
}

static evaluation_t evaluate(const image_t *img) {
  evaluation_t eval;
  run_result_t first = run_once(img);
  run_result_t second = run_once(img);

  memset(&eval, 0, sizeof(eval));
  if (first.outcome == OUTCOME_TIMEOUT || second.outcome == OUTCOME_TIMEOUT) {
    eval.outcome = OUTCOME_TIMEOUT;
  } else if (first.outcome == OUTCOME_CRASH || second.outcome == OUTCOME_CRASH) {
    eval.outcome = OUTCOME_CRASH;
  } else if (first.outcome == OUTCOME_RUNTIME_ERROR ||
             second.outcome == OUTCOME_RUNTIME_ERROR) {
    eval.outcome = OUTCOME_RUNTIME_ERROR;
  } else if (first.outcome == OUTCOME_HARNESS_ERROR ||
             second.outcome == OUTCOME_HARNESS_ERROR) {
    eval.outcome = OUTCOME_HARNESS_ERROR;
  } else if (first.outcome == OUTCOME_TRACE_ERROR ||
             second.outcome == OUTCOME_TRACE_ERROR) {
    eval.outcome = OUTCOME_TRACE_ERROR;
  } else if (first.outcome == OUTCOME_NONFINITE ||
             second.outcome == OUTCOME_NONFINITE) {
    eval.outcome = OUTCOME_NONFINITE;
  } else if (!same_encoded(&first, &second)) {
    eval.outcome = OUTCOME_NONDETERMINISTIC;
  } else {
    eval.outcome = OUTCOME_OK;
  }

  if (first.diag[0] != '\0') {
    strncpy(eval.diag, first.diag, sizeof(eval.diag) - 1);
  } else if (second.diag[0] != '\0') {
    strncpy(eval.diag, second.diag, sizeof(eval.diag) - 1);
  }
  eval.diag[sizeof(eval.diag) - 1] = '\0';

  run_result_free(&first);
  run_result_free(&second);
  return eval;
}

static int image_crop(image_t *img) {
  int x0 = img->w;
  int y0 = img->h;
  int x1 = -1;
  int y1 = -1;
  int x;
  int y;
  unsigned char *pixels;

  for (y = 0; y < img->h; y++) {
    for (x = 0; x < img->w; x++) {
      if (image_get(img, x, y)) {
        if (x < x0) {
          x0 = x;
        }
        if (x > x1) {
          x1 = x;
        }
        if (y < y0) {
          y0 = y;
        }
        if (y > y1) {
          y1 = y;
        }
      }
    }
  }
  if (x1 < 0) {
    return 0;
  }
  if (x0 == 0 && y0 == 0 && x1 == img->w - 1 && y1 == img->h - 1) {
    return 1;
  }

  pixels = (unsigned char *)calloc((size_t)(x1 - x0 + 1) * (size_t)(y1 - y0 + 1), 1);
  if (pixels == NULL) {
    return 0;
  }
  for (y = y0; y <= y1; y++) {
    for (x = x0; x <= x1; x++) {
      if (image_get(img, x, y)) {
        pixels[(size_t)(y - y0) * (size_t)(x1 - x0 + 1) + (size_t)(x - x0)] = 1;
      }
    }
  }
  free(img->pixels);
  img->pixels = pixels;
  img->w = x1 - x0 + 1;
  img->h = y1 - y0 + 1;
  return 1;
}

static int try_candidate(image_t *current, image_t *candidate,
                         outcome_t target) {
  evaluation_t eval;

  if (image_count(candidate) == 0) {
    image_free(candidate);
    return 0;
  }
  image_crop(candidate);
  eval = evaluate(candidate);
  if (eval.outcome == target) {
    image_free(current);
    *current = *candidate;
    return 1;
  }
  image_free(candidate);
  return 0;
}

static void minimize_axis(image_t *current, outcome_t target, int vertical) {
  int granularity = 2;

  for (;;) {
    int length = vertical ? current->h : current->w;
    int chunk = (length + granularity - 1) / granularity;
    int start;
    int changed = 0;

    if (chunk < 1) {
      chunk = 1;
    }
    for (start = 0; start < length; start += chunk) {
      int end = imin(length, start + chunk);
      image_t candidate;
      int y;
      int x;

      if (!image_clone(&candidate, current)) {
        return;
      }
      for (y = 0; y < candidate.h; y++) {
        for (x = 0; x < candidate.w; x++) {
          if ((vertical && y >= start && y < end) ||
              (!vertical && x >= start && x < end)) {
            image_set(&candidate, x, y, 0);
          }
        }
      }
      if (try_candidate(current, &candidate, target)) {
        changed = 1;
        if (granularity > 2) {
          granularity--;
        }
        break;
      }
    }
    if (!changed) {
      if (granularity >= length) {
        break;
      }
      granularity = imin(length, granularity * 2);
    }
  }
}

static void minimize_pixels(image_t *current, outcome_t target) {
  int granularity = 2;

  for (;;) {
    size_t size = (size_t)current->w * (size_t)current->h;
    int *positions;
    int count = 0;
    int chunk;
    int start;
    int changed = 0;
    size_t i;

    positions = (int *)malloc(size * sizeof(int));
    if (positions == NULL) {
      return;
    }
    for (i = 0; i < size; i++) {
      if (current->pixels[i]) {
        positions[count++] = (int)i;
      }
    }
    if (count <= 1) {
      free(positions);
      break;
    }
    chunk = (count + granularity - 1) / granularity;
    if (chunk < 1) {
      chunk = 1;
    }
    for (start = 0; start < count; start += chunk) {
      int end = imin(count, start + chunk);
      image_t candidate;
      int j;

      if (!image_clone(&candidate, current)) {
        free(positions);
        return;
      }
      for (j = start; j < end; j++) {
        int pos = positions[j];
        candidate.pixels[pos] = 0;
      }
      if (try_candidate(current, &candidate, target)) {
        changed = 1;
        if (granularity > 2) {
          granularity--;
        }
        break;
      }
    }
    free(positions);
    if (!changed) {
      if (granularity >= count) {
        break;
      }
      granularity = imin(count, granularity * 2);
    }
  }

  for (;;) {
    size_t size = (size_t)current->w * (size_t)current->h;
    size_t i;
    int removed = 0;

    for (i = 0; i < size; i++) {
      image_t candidate;
      if (!current->pixels[i]) {
        continue;
      }
      if (!image_clone(&candidate, current)) {
        return;
      }
      candidate.pixels[i] = 0;
      if (try_candidate(current, &candidate, target)) {
        removed = 1;
        break;
      }
    }
    if (!removed) {
      break;
    }
  }
}

static image_t minimize_image(const image_t *original, outcome_t target) {
  image_t current;
  image_t cropped;

  if (!image_clone(&current, original)) {
    return (image_t){0};
  }
  if (image_clone(&cropped, &current) && image_crop(&cropped) &&
      evaluate(&cropped).outcome == target) {
    image_free(&current);
    current = cropped;
  } else {
    image_free(&cropped);
  }

  minimize_axis(&current, target, 0);
  minimize_axis(&current, target, 1);
  minimize_pixels(&current, target);
  minimize_axis(&current, target, 0);
  minimize_axis(&current, target, 1);
  return current;
}

static int write_pbm(const char *path, const image_t *img) {
  FILE *file = fopen(path, "wb");
  int row_bytes;
  unsigned char *row;
  int x;
  int y;

  if (file == NULL) {
    return 0;
  }
  row_bytes = (img->w + 7) / 8;
  row = (unsigned char *)calloc((size_t)row_bytes, 1);
  if (row == NULL) {
    fclose(file);
    return 0;
  }
  fprintf(file, "P4\n%d %d\n", img->w, img->h);
  for (y = 0; y < img->h; y++) {
    memset(row, 0, (size_t)row_bytes);
    for (x = 0; x < img->w; x++) {
      if (image_get(img, x, y)) {
        row[x / 8] |= (unsigned char)(0x80u >> (x % 8));
      }
    }
    fwrite(row, 1, (size_t)row_bytes, file);
  }
  free(row);
  fclose(file);
  return 1;
}

static void print_ascii(FILE *file, const image_t *img) {
  int x;
  int y;

  fprintf(file, "%dx%d\n", img->w, img->h);
  for (y = 0; y < img->h; y++) {
    for (x = 0; x < img->w; x++) {
      fputc(image_get(img, x, y) ? '#' : '.', file);
    }
    fputc('\n', file);
  }
}

static void write_counterexample(const char *directory, const image_t *original,
                                 const image_t *minimal, outcome_t outcome,
                                 const char *diag) {
  char pbm_path[1024];
  char txt_path[1024];
  FILE *txt;
  int original_pixels = image_count(original);
  int minimal_pixels = image_count(minimal);

  snprintf(pbm_path, sizeof(pbm_path), "%s/counterexample-%03d-%s.pbm",
           directory, original->index, outcome_name(outcome));
  snprintf(txt_path, sizeof(txt_path), "%s/counterexample-%03d-%s.txt",
           directory, original->index, outcome_name(outcome));
  write_pbm(pbm_path, minimal);

  txt = fopen(txt_path, "w");
  if (txt == NULL) {
    return;
  }
  fprintf(txt, "potrace property test counterexample\n");
  fprintf(txt, "case index:    %d\n", original->index);
  fprintf(txt, "family:        %s\n", family_name(original->family));
  fprintf(txt, "failure:       %s\n", outcome_name(outcome));
  if (diag != NULL && diag[0] != '\0') {
    fprintf(txt, "diagnostic:    %s\n", diag);
  }
  fprintf(txt, "original size: %dx%d (%d set pixels)\n", original->w,
          original->h, original_pixels);
  fprintf(txt, "minimal size:  %dx%d (%d set pixels)\n", minimal->w,
          minimal->h, minimal_pixels);
  fprintf(txt, "parameters:    turdsize=%d turnpolicy=%d alphamax=%g "
               "opticurve=%d opttolerance=%g\n",
          original->params.turdsize, original->params.turnpolicy,
          original->params.alphamax, original->params.opticurve,
          original->params.opttolerance);
  fprintf(txt, "minimal 1-minimal: every remaining set pixel is required\n");
  fprintf(txt, "\nminimal bitmap:\n");
  print_ascii(txt, minimal);
  fclose(txt);
}

static void print_usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s [--count N] [--seed HEX] [--artifacts DIR] "
          "[--no-minimize]\n",
          argv0);
}

int main(int argc, char **argv) {
  int count = DEFAULT_CASES;
  int no_minimize = 0;
  uint64_t seed = DEFAULT_SEED;
  const char *artifact_dir = "property-artifacts";
  int family_total[FAMILY_COUNT] = {0};
  int family_fail[FAMILY_COUNT] = {0};
  int outcome_total[OUTCOME_COUNT] = {0};
  int failures = 0;
  int i;
  FILE *summary;
  char summary_path[1024];

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--count") == 0 && i + 1 < argc) {
      char *end = NULL;
      long value = strtol(argv[++i], &end, 10);
      if (end == argv[i] || *end != '\0' || value < 0 || value > 100000) {
        print_usage(argv[0]);
        return 2;
      }
      count = (int)value;
    } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      char *end = NULL;
      unsigned long long value = strtoull(argv[++i], &end, 0);
      if (end == argv[i] || *end != '\0') {
        print_usage(argv[0]);
        return 2;
      }
      seed = (uint64_t)value;
    } else if (strcmp(argv[i], "--artifacts") == 0 && i + 1 < argc) {
      artifact_dir = argv[++i];
    } else if (strcmp(argv[i], "--no-minimize") == 0) {
      no_minimize = 1;
    } else {
      print_usage(argv[0]);
      return 2;
    }
  }

  if (mkdir(artifact_dir, 0777) != 0 && errno != EEXIST) {
    fprintf(stderr, "cannot create artifact directory %s: %s\n", artifact_dir,
            strerror(errno));
    return 2;
  }
  snprintf(summary_path, sizeof(summary_path), "%s/summary.txt", artifact_dir);
  summary = fopen(summary_path, "w");
  if (summary == NULL) {
    fprintf(stderr, "cannot write %s: %s\n", summary_path, strerror(errno));
    return 2;
  }

  printf("potrace property test: cases=%d runs=%d seed=0x%016llx\n", count,
         count * 2, (unsigned long long)seed);
  fprintf(summary, "potrace property test: cases=%d runs=%d seed=0x%016llx\n",
          count, count * 2, (unsigned long long)seed);

  for (i = 0; i < count; i++) {
    image_t original;
    evaluation_t eval;

    if (!generate_case(i, seed, &original)) {
      fprintf(stderr, "case %d: generation failed\n", i);
      failures++;
      outcome_total[OUTCOME_HARNESS_ERROR]++;
      continue;
    }
    family_total[original.family]++;
    eval = evaluate(&original);
    outcome_total[eval.outcome]++;
    if (eval.outcome != OUTCOME_OK) {
      image_t minimal;
      char pbm_path[1024];
      char txt_path[1024];

      failures++;
      family_fail[original.family]++;
      if (no_minimize) {
        image_clone(&minimal, &original);
      } else {
        minimal = minimize_image(&original, eval.outcome);
      }
      printf("\ncase %03d family=%s failure=%s original=%dx%d\n", i,
             family_name(original.family), outcome_name(eval.outcome),
             original.w, original.h);
      if (eval.diag[0] != '\0') {
        printf("diagnostic: %s\n", eval.diag);
      }
      printf("minimal bitmap: %dx%d (%d set pixels)\n", minimal.w, minimal.h,
             image_count(&minimal));
      print_ascii(stdout, &minimal);
      write_counterexample(artifact_dir, &original, &minimal, eval.outcome,
                           eval.diag);
      snprintf(pbm_path, sizeof(pbm_path), "%s/counterexample-%03d-%s.pbm",
               artifact_dir, i, outcome_name(eval.outcome));
      snprintf(txt_path, sizeof(txt_path), "%s/counterexample-%03d-%s.txt",
               artifact_dir, i, outcome_name(eval.outcome));
      printf("saved: %s\n", pbm_path);
      printf("saved: %s\n", txt_path);
      image_free(&minimal);
    }
    image_free(&original);
  }

  printf("\nsummary\n");
  fprintf(summary, "\nsummary\n");
  printf("  passed=%d failed=%d\n", count - failures, failures);
  fprintf(summary, "  passed=%d failed=%d\n", count - failures, failures);
  printf("  crashes=%d timeouts=%d nonfinite=%d nondeterministic=%d\n",
         outcome_total[OUTCOME_CRASH], outcome_total[OUTCOME_TIMEOUT],
         outcome_total[OUTCOME_NONFINITE],
         outcome_total[OUTCOME_NONDETERMINISTIC]);
  fprintf(summary,
          "  crashes=%d timeouts=%d nonfinite=%d nondeterministic=%d\n",
          outcome_total[OUTCOME_CRASH], outcome_total[OUTCOME_TIMEOUT],
          outcome_total[OUTCOME_NONFINITE],
          outcome_total[OUTCOME_NONDETERMINISTIC]);
  printf("  trace_errors=%d runtime_errors=%d harness_errors=%d\n",
         outcome_total[OUTCOME_TRACE_ERROR],
         outcome_total[OUTCOME_RUNTIME_ERROR],
         outcome_total[OUTCOME_HARNESS_ERROR]);
  fprintf(summary,
          "  trace_errors=%d runtime_errors=%d harness_errors=%d\n",
          outcome_total[OUTCOME_TRACE_ERROR],
          outcome_total[OUTCOME_RUNTIME_ERROR],
          outcome_total[OUTCOME_HARNESS_ERROR]);
  printf("  families: holes=%d/%d pixel=%d/%d collinear=%d/%d "
         "oversized=%d/%d (total/failed)\n",
         family_total[FAMILY_HOLES], family_fail[FAMILY_HOLES],
         family_total[FAMILY_PIXEL], family_fail[FAMILY_PIXEL],
         family_total[FAMILY_COLLINEAR], family_fail[FAMILY_COLLINEAR],
         family_total[FAMILY_OVERSIZED], family_fail[FAMILY_OVERSIZED]);
  fprintf(summary,
          "  families: holes=%d/%d pixel=%d/%d collinear=%d/%d "
          "oversized=%d/%d (total/failed)\n",
          family_total[FAMILY_HOLES], family_fail[FAMILY_HOLES],
          family_total[FAMILY_PIXEL], family_fail[FAMILY_PIXEL],
          family_total[FAMILY_COLLINEAR], family_fail[FAMILY_COLLINEAR],
          family_total[FAMILY_OVERSIZED], family_fail[FAMILY_OVERSIZED]);
  fclose(summary);

  return failures == 0 ? 0 : 1;
}
