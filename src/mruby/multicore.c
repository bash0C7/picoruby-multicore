/* mruby bindings for Multicore (included by ../multicore.c when PICORB_VM_MRUBY).
 *
 * The C side never raises: _open returns a status Integer, _send and _close
 * return booleans, _take returns nil or the result. mrblib/multicore.rb turns
 * those into Multicore::Error, so both VMs share one set of messages.
 */
#include <mruby.h>
#include <mruby/presym.h>
#include <mruby/class.h>

#include "../../include/multicore.h"

static mrb_value
mrb_multicore__open(mrb_state *mrb, mrb_value self)
{
  const char *unit;
  mrb_get_args(mrb, "z", &unit);
  return mrb_fixnum_value(MULTICORE_open(unit));
}

static mrb_value
mrb_multicore__send(mrb_state *mrb, mrb_value self)
{
  mrb_int n;
  mrb_get_args(mrb, "i", &n);
  if (n < 0 || MULTICORE_JOB_MAX < n) {
    return mrb_false_value();
  }
  return mrb_bool_value(MULTICORE_try_send((int32_t)n));
}

static mrb_value
mrb_multicore__take(mrb_state *mrb, mrb_value self)
{
  int32_t out;
  if (!MULTICORE_try_receive(&out)) {
    return mrb_nil_value();
  }
  return mrb_fixnum_value((mrb_int)out);
}

static mrb_value
mrb_multicore__close(mrb_state *mrb, mrb_value self)
{
  return mrb_bool_value(MULTICORE_close());
}

void
mrb_picoruby_multicore_gem_init(mrb_state *mrb)
{
  /* gem init runs before mrblib is evaluated, so the class is defined here
   * (mrb_class_get_id would not find it yet) and mrblib reopens it. */
  struct RClass *class_Multicore = mrb_define_class_id(mrb, MRB_SYM(Multicore), mrb->object_class);
  mrb_define_method_id(mrb, class_Multicore, MRB_SYM(_open), mrb_multicore__open, MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, class_Multicore, MRB_SYM(_send), mrb_multicore__send, MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, class_Multicore, MRB_SYM(_take), mrb_multicore__take, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, class_Multicore, MRB_SYM(_close), mrb_multicore__close, MRB_ARGS_NONE());
}

void
mrb_picoruby_multicore_gem_final(mrb_state *mrb)
{
  (void)mrb;
  MULTICORE_close();
}
