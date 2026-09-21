/* Host-side check of the engine and the pthread port without a VM: allocation
 * accounting, the failure path and close-while-busy. Built and run under
 * ThreadSanitizer and AddressSanitizer by run.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "multicore.h"

extern int multicore_host_live_allocs(void);
extern void multicore_host_fail_after(int n);

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static const uint8_t add_in[] = { 0x92, 0x01, 0x02 };
static const uint8_t stamp_in[] = { 0x90 };
static const uint8_t slow_in[] = { 0x91, 0xd1, 0x09, 0x60 };  /* [2400] ms */

static void
wait_done(int32_t id)
{
  while (MULTICORE_poll(id) == MULTICORE_JOB_PENDING) {
    usleep(100);
  }
}

int
main(void)
{
  int round, i;

  CHECK(multicore_host_live_allocs() == 0);

  /* start / submit / forget / stop cycles: nothing is held between cycles, one block while running */
  for (round = 0; round < 200; round++) {
    int32_t ids[MULTICORE_QUEUE_DEPTH];
    CHECK(MULTICORE_start() == MULTICORE_OK);
    CHECK(multicore_host_live_allocs() == 1);
    for (i = 0; i < MULTICORE_QUEUE_DEPTH; i++) {
      ids[i] = i % 2 ? MULTICORE_submit("stamp", stamp_in, 1) : MULTICORE_submit("add", add_in, 3);
      CHECK(ids[i] >= 0);
    }
    CHECK(MULTICORE_submit("add", add_in, 3) == MULTICORE_E_QUEUE_FULL);
    if (round % 2) MULTICORE_forget(ids[1]);
    for (i = 0; i < MULTICORE_QUEUE_DEPTH; i++) {
      const uint8_t *d;
      int32_t len, st;
      if (round % 2 && i == 1) continue;
      wait_done(ids[i]);
      CHECK(MULTICORE_result(ids[i], &d, &len, &st) == 0 && st >= 0);
      MULTICORE_release(ids[i]);
    }
    CHECK(MULTICORE_stop());
    CHECK(multicore_host_live_allocs() == 0);
    CHECK(MULTICORE_poll(ids[0]) == MULTICORE_JOB_UNKNOWN);  /* the slots are gone */
  }

  /* an allocation that fails leaves nothing allocated and nothing running */
  multicore_host_fail_after(0);
  CHECK(MULTICORE_start() == MULTICORE_NO_MEMORY);
  CHECK(!MULTICORE_running());
  CHECK(multicore_host_live_allocs() == 0);
  multicore_host_fail_after(-1);
  CHECK(MULTICORE_start() == MULTICORE_OK);
  CHECK(MULTICORE_stop());
  CHECK(multicore_host_live_allocs() == 0);

  /* close while a kernel runs: stop reports busy, the memory stays valid until the worker is done */
  CHECK(MULTICORE_start() == MULTICORE_OK);
  {
    int32_t id = MULTICORE_submit("slow", slow_in, 4);
    CHECK(id >= 0);
    usleep(100000);  /* let the worker pick the job up */
    CHECK(!MULTICORE_stop());
    CHECK(multicore_host_live_allocs() == 1);
    CHECK(MULTICORE_start() == MULTICORE_CORE_BUSY);
    CHECK(multicore_host_live_allocs() == 1);
    sleep(1);
    CHECK(MULTICORE_stop());
    CHECK(multicore_host_live_allocs() == 0);
  }

  printf(failures ? "FAILED\n" : "ok\n");
  return failures != 0;
}
