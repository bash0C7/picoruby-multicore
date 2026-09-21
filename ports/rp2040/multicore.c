/* rp2040 port: unit "core1_lcg" (Pico 2 W / RP2350 and RP2040).
 *
 * Core 1 runs the kernel and nothing else. The worker is RAM resident
 * (__not_in_flash_func) and only touches the inter-core FIFO through the
 * pico-sdk's inline accessors, so it never calls into flash, btstack, cyw43 or
 * lwIP. Reading the BOOTSEL button drives the QSPI chip select and stays on
 * core 0.
 *
 * Core 1 is a single resource: picoruby-psg launches it too. MULTICORE_open
 * probes the inter-core FIFO first and reports MULTICORE_CORE_BUSY when core 1
 * is already running someone else's code, instead of resetting it from under
 * them.
 *
 * This file needs the pico-sdk include paths, so it is not compiled into
 * libmruby: the firmware build definition adds it to the CMake source list.
 */
#include <string.h>
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/structs/sio.h"

#include "../../include/multicore.h"

/* Jobs are 0..0x7fffffff, so the high bit marks the control words. */
#define CMD_STOP     0xffffffffu
#define ACK_READY    0x80000001u
#define ACK_STOPPED  0x80000002u

#define READY_TIMEOUT_US  100000
#define STOP_TIMEOUT_US   500000

static bool running;

static void
__not_in_flash_func(multicore_worker_main)(void)
{
  multicore_fifo_push_blocking_inline(ACK_READY);
  while (true) {
    while (!multicore_fifo_rvalid()) {
      tight_loop_contents();
    }
    uint32_t word = sio_hw->fifo_rd;
    if (word == CMD_STOP) {
      multicore_fifo_push_blocking_inline(ACK_STOPPED);
      /* Core 0 resets this core right after the ack. */
      while (true) {
        tight_loop_contents();
      }
    }
    multicore_fifo_push_blocking_inline((uint32_t)MULTICORE_lcg((int32_t)word));
  }
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
MULTICORE_open(const char *unit)
{
  if (strcmp(unit, "core1_lcg") != 0) {
    return MULTICORE_UNKNOWN_UNIT;
  }
  if (running) {
    return MULTICORE_CORE_BUSY;
  }
  if (!core1_is_free()) {
    return MULTICORE_CORE_BUSY;
  }
  multicore_fifo_drain();
  multicore_launch_core1(multicore_worker_main);
  uint32_t ack;
  if (!multicore_fifo_pop_timeout_us(READY_TIMEOUT_US, &ack) || ack != ACK_READY) {
    multicore_reset_core1();
    multicore_fifo_drain();
    return MULTICORE_START_FAILED;
  }
  running = true;
  return MULTICORE_OK;
}

bool
MULTICORE_try_send(int32_t n)
{
  if (!running || !multicore_fifo_wready()) {
    return false;
  }
  sio_hw->fifo_wr = (uint32_t)n;
  __sev();
  return true;
}

bool
MULTICORE_try_receive(int32_t *out)
{
  if (!running || !multicore_fifo_rvalid()) {
    return false;
  }
  *out = (int32_t)sio_hw->fifo_rd;
  return true;
}

bool
MULTICORE_close(void)
{
  if (!running) {
    return true;
  }
  if (!multicore_fifo_push_timeout_us(CMD_STOP, STOP_TIMEOUT_US)) {
    return false;
  }
  uint32_t word;
  /* Results queued before the stop word come out first; drop them. */
  while (multicore_fifo_pop_timeout_us(STOP_TIMEOUT_US, &word)) {
    if (word == ACK_STOPPED) {
      multicore_reset_core1();
      multicore_fifo_drain();
      running = false;
      return true;
    }
  }
  return false;
}
