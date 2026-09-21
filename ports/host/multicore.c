/* host port: unit "host_lcg".
 *
 * There is no second core to move the job to, so the kernel runs on the
 * calling thread: _try_send computes the result and queues it, _try_receive
 * hands it back. Both stay non-blocking, so Ruby code written against a board
 * port runs unchanged in the host tests (the take loop just finishes on its
 * first turn).
 */
#include <string.h>

#include "../../include/multicore.h"

#define QUEUE_DEPTH 8

static bool running;
static int32_t results[QUEUE_DEPTH];
static int head;
static int tail;
static int queued;

int
MULTICORE_open(const char *unit)
{
  if (strcmp(unit, "host_lcg") != 0) {
    return MULTICORE_UNKNOWN_UNIT;
  }
  if (running) {
    return MULTICORE_CORE_BUSY;
  }
  head = 0;
  tail = 0;
  queued = 0;
  running = true;
  return MULTICORE_OK;
}

bool
MULTICORE_try_send(int32_t n)
{
  if (!running || queued == QUEUE_DEPTH) {
    return false;
  }
  results[tail] = MULTICORE_lcg(n);
  tail = (tail + 1) % QUEUE_DEPTH;
  queued++;
  return true;
}

bool
MULTICORE_try_receive(int32_t *out)
{
  if (!running || queued == 0) {
    return false;
  }
  *out = results[head];
  head = (head + 1) % QUEUE_DEPTH;
  queued--;
  return true;
}

bool
MULTICORE_close(void)
{
  running = false;
  queued = 0;
  head = 0;
  tail = 0;
  return true;
}
