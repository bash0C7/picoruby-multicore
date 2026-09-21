/* picoruby-multicore — VM bindings only.
 *
 * This file is compiled into libmruby, which does not carry the board SDK's
 * include paths, so it must not include any SDK header. The worker itself
 * (MULTICORE_open / _try_send / _try_receive / _close, declared in
 * include/multicore.h) lives in ports/<board>/multicore.c and is compiled by
 * the board's firmware build definition (the host port is compiled here).
 */
#include "../include/multicore.h"

#if defined(PICORB_VM_MRUBY)
#include "mruby/multicore.c"
#elif defined(PICORB_VM_MRUBYC)
#include "mrubyc/multicore.c"
#endif
