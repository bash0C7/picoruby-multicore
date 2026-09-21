/* picoruby-multicore: VM glue only.
 *
 * This file is compiled into libmruby, which does not carry the board SDK's
 * include paths, so it must not include any SDK header. The worker (the
 * MULTICORE_* functions declared in include/multicore.h) lives in
 * ports/<board>/multicore.c and is compiled by the board's firmware build
 * definition (the host port is compiled here).
 *
 * The glue is thin and never raises: every method returns a status, a String
 * or nil, and mrblib/multicore.rb turns those into Ruby values and
 * Multicore::Error, so both VMs share one Ruby layer. Float <-> bytes lives
 * here because neither VM can convert a Float's bits from Ruby.
 */
#include <string.h>

#include "../include/multicore.h"

#define MC_NAME_MAX 64
#define MC_NAMES_MAX 512

/* Float64 (or float32) big endian bytes <-> double. */
static void
mc_put_f64(double d, uint8_t out[8])
{
  uint64_t u;
  int i;
  memcpy(&u, &d, sizeof u);
  for (i = 7; i >= 0; i--) {
    out[i] = (uint8_t)(u & 0xff);
    u >>= 8;
  }
}

static bool
mc_get_float(const uint8_t *p, int len, double *d)
{
  int i;
  if (len == 8) {
    uint64_t u = 0;
    for (i = 0; i < 8; i++) {
      u = (u << 8) | p[i];
    }
    memcpy(d, &u, sizeof u);
    return true;
  }
  if (len == 4) {
    uint32_t u = 0;
    float f;
    for (i = 0; i < 4; i++) {
      u = (u << 8) | p[i];
    }
    memcpy(&f, &u, sizeof f);
    *d = (double)f;
    return true;
  }
  return false;
}

#if defined(PICORB_VM_MRUBY)
#include "mruby/multicore.c"
#elif defined(PICORB_VM_MRUBYC)
#include "mrubyc/multicore.c"
#endif
