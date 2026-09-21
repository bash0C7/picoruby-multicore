/* rp2040 port (Pico 2 W / RP2350 and RP2040): core 1 runs the kernels.
 *
 * The worker loop and the dispatch (mc_worker_run_one) are RAM resident
 * (__not_in_flash_func). The two cores talk through the shared slots
 * (include/multicore_engine.h): a state flag plus memory barriers (__dmb), and
 * SEV / WFE as the doorbell. The hardware FIFO is not used to carry anything;
 * it is only probed once to see whether core 1 is free and drained, and no FIFO
 * word ever reaches Ruby.
 *
 * Core 1 is a single resource: picoruby-psg launches it too. MULTICORE_start
 * probes it first and reports MULTICORE_CORE_BUSY when it is already running
 * someone else's code, instead of resetting it from under them.
 *
 * Reading the BOOTSEL button drives the QSPI chip select and stays on core 0.
 *
 * This file needs the pico-sdk include paths, so it is not compiled into
 * libmruby: the firmware build definition adds it to the CMake source list.
 * MULTICORE_STACK_BYTES is core 1's stack (default 8192), and it and the job slots are malloc'd in
 * MULTICORE_start and freed in MULTICORE_stop once core 1 has parked; the worker never allocates. */
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"

#include "../../include/multicore.h"

#define READY_TIMEOUT_US  100000
#define STOP_TIMEOUT_US   500000

static volatile bool stop_requested;
static volatile bool worker_ready;
static volatile bool worker_parked;
static bool started;
/* Core 1's stack is malloc'd in MULTICORE_start: multicore_launch_core1_with_stack wants the lowest
 * address (word aligned; 8 bytes here to honour the AAPCS) and a size in bytes (a multiple of 4; here
 * of 8). core1_stack_raw is what malloc returned and what gets freed. */
#define CORE1_STACK_BYTES (MULTICORE_STACK_BYTES & ~(size_t)7)
static void *core1_stack_raw;
static uint32_t *core1_stack;

#define MC_BARRIER() __dmb()
#define MC_WAKE() __sev()
#define MC_FUNC(name) __not_in_flash_func(name)
#include "../../include/multicore_engine.h"

static void
__not_in_flash_func(worker_main)(void)
{
  worker_ready = true;
  MC_BARRIER();
  while (!stop_requested) {
    if (!mc_worker_run_one()) {
      /* SEV sets the event flag even when core 1 is not waiting yet, so a job
       * queued between the scan and this WFE is not missed. */
      __wfe();
    }
  }
  worker_parked = true;
  MC_BARRIER();
  /* Core 0 resets this core once it sees worker_parked. */
  while (true) {
    tight_loop_contents();
  }
}

uint32_t
MULTICORE_now_ms(void)
{
  return to_ms_since_boot(get_absolute_time());
}

/* True when core 1 sits in the bootrom's wait-for-vector loop, which echoes
 * every word it receives. Code launched on core 1 (picoruby-psg's audio loop,
 * or our own worker) does not echo, so the read times out. Both the push and
 * the pop are bounded, so this never hangs the VM. */
static bool
core1_is_free(void)
{
  uint32_t echo;
  multicore_fifo_drain();
  __sev();
  if (!multicore_fifo_push_timeout_us(0, 1000)) {
    return false;
  }
  if (!multicore_fifo_pop_timeout_us(2000, &echo)) {
    return false;
  }
  return echo == 0;
}

int
MULTICORE_start(void)
{
  if (mc_running) {
    return MULTICORE_OK;
  }
  if (started || !core1_is_free()) {
    return MULTICORE_CORE_BUSY;
  }
  if (!mc_alloc_slots()) {
    return MULTICORE_NO_MEMORY;
  }
  core1_stack_raw = malloc(CORE1_STACK_BYTES + 8);
  if (core1_stack_raw == NULL) {
    mc_free_slots();
    return MULTICORE_NO_MEMORY;
  }
  core1_stack = (uint32_t *)(((uintptr_t)core1_stack_raw + 7u) & ~(uintptr_t)7u);
  multicore_fifo_drain();
  stop_requested = false;
  worker_ready = false;
  worker_parked = false;
  MC_BARRIER();
  multicore_launch_core1_with_stack(worker_main, core1_stack, CORE1_STACK_BYTES);
  uint64_t deadline = time_us_64() + READY_TIMEOUT_US;
  while (!worker_ready) {
    if (time_us_64() > deadline) {
      multicore_reset_core1();
      multicore_fifo_drain();
      free(core1_stack_raw);
      core1_stack_raw = NULL;
      mc_free_slots();
      return MULTICORE_START_FAILED;
    }
    tight_loop_contents();
  }
  multicore_fifo_drain();
  started = true;
  mc_running = true;
  return MULTICORE_OK;
}

bool
MULTICORE_stop(void)
{
  if (!started) {
    mc_running = false;
    return true;
  }
  mc_running = false;
  stop_requested = true;
  MC_BARRIER();
  __sev();
  uint64_t deadline = time_us_64() + STOP_TIMEOUT_US;
  while (!worker_parked) {
    if (time_us_64() > deadline) {
      return false;
    }
    tight_loop_contents();
  }
  multicore_reset_core1();
  multicore_fifo_drain();
  started = false;
  free(core1_stack_raw);
  core1_stack_raw = NULL;
  mc_free_slots();
  return true;
}
