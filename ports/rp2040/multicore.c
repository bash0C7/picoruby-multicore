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
 * MULTICORE_STACK_BYTES is core 1's stack (default 8192). */
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
static uint32_t core1_stack[MULTICORE_STACK_BYTES / 4] __attribute__((aligned(8)));

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
  multicore_fifo_drain();
  mc_reset_slots();
  stop_requested = false;
  worker_ready = false;
  worker_parked = false;
  MC_BARRIER();
  multicore_launch_core1_with_stack(worker_main, core1_stack, sizeof(core1_stack));
  uint64_t deadline = time_us_64() + READY_TIMEOUT_US;
  while (!worker_ready) {
    if (time_us_64() > deadline) {
      multicore_reset_core1();
      multicore_fifo_drain();
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
  mc_reset_slots();
  return true;
}
