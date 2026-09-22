/* The job slots and the worker step, shared by every port.
 *
 * A port's multicore.c defines the primitives below, includes this header and
 * then adds MULTICORE_start / MULTICORE_stop / MULTICORE_now_ms and its worker
 * loop. It is a header (not a compiled unit) so that on rp2040 the worker path
 * lands in the port's own translation unit and can be placed in RAM with
 * MC_FUNC.
 *
 *   MC_BARRIER()  full memory barrier
 *   MC_WAKE()     tell the worker there is work (after the barrier)
 *   MC_FUNC(f)    optional: wraps the name of every function the worker runs
 *
 * Ownership: core 0 (the VM) is the only writer of a slot in the FREE and
 * DONE states and the only one that queues; the worker owns a slot from
 * QUEUED until it stores DONE (or FREE for a forgotten job). `state` is what
 * hands ownership over, always after a barrier. */
#ifndef PICORUBY_MULTICORE_ENGINE_H_
#define PICORUBY_MULTICORE_ENGINE_H_

#include <stdlib.h>
#include <string.h>
#include "multicore.h"

#ifndef MC_BARRIER
#define MC_BARRIER() __sync_synchronize()
#endif
/* The slots are allocated on core 0, before the worker starts; the worker never allocates. */
#ifndef MC_MALLOC
#define MC_MALLOC(n) malloc(n)
#endif
#ifndef MC_FREE
#define MC_FREE(p) free(p)
#endif
#ifndef MC_FUNC
#define MC_FUNC(name) name
#endif
#ifndef MC_WAKE
#error "the port must define MC_WAKE before including multicore_engine.h"
#endif

#define MULTICORE_MAX_KERNELS 64  /* kernels whose init is tracked; later ones init on every call */

/* state and forget cross between the cores: acquire on read, release on write. */
#define MC_GET(v)    __atomic_load_n(&(v), __ATOMIC_ACQUIRE)
#define MC_SET(v, x) __atomic_store_n(&(v), (x), __ATOMIC_RELEASE)

enum { MC_FREE = 0, MC_QUEUED = 1, MC_RUNNING = 2, MC_DONE = 3 };

typedef struct {
  int32_t state;
  int32_t forget;
  uint32_t seq;
  int32_t kernel;
  int32_t in_len;
  int32_t status;
  int32_t out_len;
  uint8_t *in;   /* MULTICORE_IN_CAP bytes, its own malloc block */
  uint8_t *out;  /* MULTICORE_OUT_CAP bytes, its own malloc block */
} mc_slot_t;

static mc_slot_t *mc_slots;  /* NULL while no worker runs */
static uint32_t mc_next_seq = 1;
static uint8_t mc_inited[MULTICORE_MAX_KERNELS];
static bool mc_running;  /* core 0 only */

/* Where the last mc_alloc_slots failed, for the port's log: what (slot array, or the input / output
 * buffer of slot `index`), how many bytes were asked and how many blocks were allocated before it. */
static struct {
  const char *what;
  int index;
  size_t bytes;
  int blocks_before;
} mc_alloc_failure;

/* The slot array and every buffer are separate small blocks, so the largest contiguous request is
 * max(MULTICORE_IN_CAP, MULTICORE_OUT_CAP) however deep the queue is. */
#define MC_BLOCKS (1 + 2 * MULTICORE_QUEUE_DEPTH)
#define MC_SLOTS_BYTES (sizeof(mc_slot_t) * MULTICORE_QUEUE_DEPTH + \
                        (size_t)MULTICORE_QUEUE_DEPTH * (MULTICORE_IN_CAP + MULTICORE_OUT_CAP))

/* Only after the worker has really stopped. */
static void
mc_free_slots(void)
{
  int i;
  if (mc_slots == NULL) {
    return;
  }
  for (i = 0; i < MULTICORE_QUEUE_DEPTH; i++) {
    if (mc_slots[i].in != NULL) {
      MC_FREE(mc_slots[i].in);
    }
    if (mc_slots[i].out != NULL) {
      MC_FREE(mc_slots[i].out);
    }
  }
  MC_FREE(mc_slots);
  mc_slots = NULL;
}

/* All slots FREE. False when a malloc fails: everything already allocated is freed and
 * mc_alloc_failure says which block it was. */
static bool
mc_alloc_slots(void)
{
  int i;
  int blocks = 0;
  mc_slots = (mc_slot_t *)MC_MALLOC(sizeof(mc_slot_t) * MULTICORE_QUEUE_DEPTH);
  if (mc_slots == NULL) {
    mc_alloc_failure.what = "slot array";
    mc_alloc_failure.index = -1;
    mc_alloc_failure.bytes = sizeof(mc_slot_t) * MULTICORE_QUEUE_DEPTH;
    mc_alloc_failure.blocks_before = 0;
    return false;
  }
  blocks++;
  memset(mc_slots, 0, sizeof(mc_slot_t) * MULTICORE_QUEUE_DEPTH);
  for (i = 0; i < MULTICORE_QUEUE_DEPTH; i++) {
    mc_slots[i].in = (uint8_t *)MC_MALLOC(MULTICORE_IN_CAP);
    if (mc_slots[i].in == NULL) {
      mc_alloc_failure.what = "input buffer";
      mc_alloc_failure.index = i;
      mc_alloc_failure.bytes = MULTICORE_IN_CAP;
      mc_alloc_failure.blocks_before = blocks;
      mc_free_slots();
      return false;
    }
    blocks++;
    mc_slots[i].out = (uint8_t *)MC_MALLOC(MULTICORE_OUT_CAP);
    if (mc_slots[i].out == NULL) {
      mc_alloc_failure.what = "output buffer";
      mc_alloc_failure.index = i;
      mc_alloc_failure.bytes = MULTICORE_OUT_CAP;
      mc_alloc_failure.blocks_before = blocks;
      mc_free_slots();
      return false;
    }
    blocks++;
  }
  MC_BARRIER();
  return true;
}

/* Runs the oldest queued job, if any, on the calling (worker) core. */
static bool
MC_FUNC(mc_worker_run_one)(void)
{
  mc_slot_t *best = NULL;
  mc_slot_t *slots = mc_slots;
  int i;
  for (i = 0; i < MULTICORE_QUEUE_DEPTH; i++) {
    mc_slot_t *s = &slots[i];
    if (MC_GET(s->state) == MC_QUEUED && (best == NULL || (int32_t)(s->seq - best->seq) < 0)) {
      best = s;
    }
  }
  if (best == NULL) {
    return false;
  }
  MC_BARRIER();
  MC_SET(best->state, MC_RUNNING);

  const multicore_kernel_t *k = &multicore_kernels[best->kernel];
  if (best->kernel >= MULTICORE_MAX_KERNELS || !mc_inited[best->kernel]) {
    k->init();
    if (best->kernel < MULTICORE_MAX_KERNELS) {
      mc_inited[best->kernel] = 1;
    }
  }
  int32_t status = k->call(best->in, best->in_len, best->out, MULTICORE_OUT_CAP);
  int32_t len = 0;
  if (status > MULTICORE_OUT_CAP) {
    status = MULTICORE_K_NOSPACE;
  }
  if (status >= 0) {
    len = status;
  } else if (status == MULTICORE_K_RAISED) {
    const char *msg = k->error_message != NULL ? k->error_message() : NULL;
    if (msg == NULL) {
      msg = "the kernel raised";
    }
    while (len < MULTICORE_OUT_CAP - 1 && msg[len] != '\0') {
      best->out[len] = (uint8_t)msg[len];
      len++;
    }
  }
  best->status = status;
  best->out_len = len;
  MC_BARRIER();
  MC_SET(best->state, MC_GET(best->forget) ? MC_FREE : MC_DONE);
  MC_BARRIER();
  return true;
}

static int
mc_find_kernel(const char *name)
{
  int i;
  for (i = 0; multicore_kernels[i].name != NULL; i++) {
    if (strcmp(multicore_kernels[i].name, name) == 0) {
      return i;
    }
  }
  return -1;
}

static mc_slot_t *
mc_find_job(int32_t job)
{
  int i;
  if (mc_slots == NULL) {
    return NULL;
  }
  for (i = 0; i < MULTICORE_QUEUE_DEPTH; i++) {
    if (MC_GET(mc_slots[i].state) != MC_FREE && (int32_t)(mc_slots[i].seq & 0x7fffffffu) == job) {
      return &mc_slots[i];
    }
  }
  return NULL;
}

bool
MULTICORE_running(void)
{
  return mc_running;
}

int32_t
MULTICORE_submit(const char *name, const uint8_t *in, int32_t in_len)
{
  if (!mc_running) {
    return MULTICORE_E_NOT_RUNNING;
  }
  int k = mc_find_kernel(name);
  if (k < 0) {
    return MULTICORE_E_UNKNOWN;
  }
  if (in_len < 0 || in_len > MULTICORE_IN_CAP) {
    return MULTICORE_E_INPUT_TOO_BIG;
  }
  mc_slot_t *slot = NULL;
  int i;
  for (i = 0; i < MULTICORE_QUEUE_DEPTH; i++) {
    if (MC_GET(mc_slots[i].state) == MC_FREE) {
      slot = &mc_slots[i];
      break;
    }
  }
  if (slot == NULL) {
    return MULTICORE_E_QUEUE_FULL;
  }
  memcpy(slot->in, in, (size_t)in_len);
  slot->in_len = in_len;
  slot->kernel = k;
  MC_SET(slot->forget, 0);
  slot->status = 0;
  slot->out_len = 0;
  slot->seq = mc_next_seq++;
  MC_BARRIER();
  MC_SET(slot->state, MC_QUEUED);
  MC_BARRIER();
  MC_WAKE();
  return (int32_t)(slot->seq & 0x7fffffffu);
}

int
MULTICORE_poll(int32_t job)
{
  mc_slot_t *s = mc_find_job(job);
  if (s == NULL) {
    return MULTICORE_JOB_UNKNOWN;
  }
  return MC_GET(s->state) == MC_DONE ? MULTICORE_JOB_DONE : MULTICORE_JOB_PENDING;
}

int
MULTICORE_result(int32_t job, const uint8_t **data, int32_t *len, int32_t *status)
{
  mc_slot_t *s = mc_find_job(job);
  if (s == NULL || MC_GET(s->state) != MC_DONE) {
    return -1;
  }
  MC_BARRIER();
  *data = s->out;
  *len = s->out_len;
  *status = s->status;
  return 0;
}

void
MULTICORE_release(int32_t job)
{
  mc_slot_t *s = mc_find_job(job);
  if (s != NULL && MC_GET(s->state) == MC_DONE) {
    MC_SET(s->state, MC_FREE);
    MC_BARRIER();
  }
}

void
MULTICORE_forget(int32_t job)
{
  mc_slot_t *s = mc_find_job(job);
  if (s == NULL) {
    return;
  }
  if (MC_GET(s->state) != MC_DONE) {
    MC_SET(s->forget, 1);
    MC_BARRIER();
  }
  /* The worker frees a forgotten job it finishes after this point; one it
   * finished before is DONE here and is freed now. */
  if (MC_GET(s->state) == MC_DONE) {
    MC_SET(s->state, MC_FREE);
    MC_BARRIER();
  }
}

const char *
MULTICORE_signature(const char *name)
{
  int k = mc_find_kernel(name);
  return k < 0 ? NULL : multicore_kernels[k].signature();
}

void
MULTICORE_names(char *buf, int cap)
{
  int n = 0;
  int i;
  buf[0] = '\0';
  for (i = 0; multicore_kernels[i].name != NULL; i++) {
    const char *name = multicore_kernels[i].name;
    int need = (int)strlen(name) + (i > 0 ? 2 : 0);
    if (n + need >= cap) {
      break;
    }
    if (i > 0) {
      buf[n++] = ',';
      buf[n++] = ' ';
    }
    memcpy(buf + n, name, strlen(name));
    n += (int)strlen(name);
    buf[n] = '\0';
  }
}

int MULTICORE_in_cap(void) { return MULTICORE_IN_CAP; }
int MULTICORE_out_cap(void) { return MULTICORE_OUT_CAP; }
int MULTICORE_queue_depth(void) { return MULTICORE_QUEUE_DEPTH; }

#endif /* PICORUBY_MULTICORE_ENGINE_H_ */
