#ifndef PICORUBY_MULTICORE_H_
#define PICORUBY_MULTICORE_H_

#include <stdbool.h>
#include <stdint.h>

/* The contract every port implements. One worker per build, one int32 in and
 * one int32 out per job. The VM stays on its own core and the worker never
 * touches it.
 *
 * The job is the LCG kernel below, so the same n gives the same result on
 * every port. */

#define MULTICORE_JOB_MAX 0x7fffffff

/* MULTICORE_open() status. The Ruby layer turns a non-zero status into
 * Multicore::Error, so no port raises by itself. */
#define MULTICORE_OK           0
#define MULTICORE_UNKNOWN_UNIT 1
#define MULTICORE_CORE_BUSY    2
#define MULTICORE_START_FAILED 3

/* n rounds of seed = (seed * 1103515245 + 12345) & 0x7fffffff, from seed 1.
 * The constants live here so every port runs the identical kernel. */
static inline int32_t
MULTICORE_lcg(int32_t n)
{
  uint32_t seed = 1;
  int32_t i = 0;
  while (i < n) {
    seed = (seed * 1103515245u + 12345u) & 0x7fffffffu;
    i++;
  }
  return (int32_t)seed;
}

/* Start the worker the unit name selects. The port owns the name table. */
int MULTICORE_open(const char *unit);

/* Queue one job. False when the input queue is full or no worker is running. */
bool MULTICORE_try_send(int32_t n);

/* Take one result. False when none is ready or no worker is running. */
bool MULTICORE_try_receive(int32_t *out);

/* Stop the worker and release the core. False when the worker is still inside
 * a job after the bounded wait; the caller may close again later. */
bool MULTICORE_close(void);

#endif /* PICORUBY_MULTICORE_H_ */
