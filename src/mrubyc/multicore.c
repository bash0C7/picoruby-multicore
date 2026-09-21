/* mruby/c bindings for Multicore (included by ../multicore.c when PICORB_VM_MRUBYC).
 *
 * Same contract as the mruby binding: the C side returns a status or a boolean
 * and never raises. mrblib/multicore.rb owns every Multicore::Error message.
 */
#include <mrubyc.h>

#include "../../include/multicore.h"

static void
c_multicore__open(mrbc_vm *vm, mrbc_value *v, int argc)
{
  (void)argc;
  const char *unit = (const char *)GET_STRING_ARG(1);
  SET_INT_RETURN(MULTICORE_open(unit));
}

static void
c_multicore__send(mrbc_vm *vm, mrbc_value *v, int argc)
{
  (void)argc;
  mrbc_int_t n = GET_INT_ARG(1);
  if (n < 0 || MULTICORE_JOB_MAX < n) {
    SET_FALSE_RETURN();
    return;
  }
  if (MULTICORE_try_send((int32_t)n)) {
    SET_TRUE_RETURN();
  } else {
    SET_FALSE_RETURN();
  }
}

static void
c_multicore__take(mrbc_vm *vm, mrbc_value *v, int argc)
{
  (void)argc;
  int32_t out;
  if (MULTICORE_try_receive(&out)) {
    SET_INT_RETURN((mrbc_int_t)out);
  } else {
    SET_NIL_RETURN();
  }
}

static void
c_multicore__close(mrbc_vm *vm, mrbc_value *v, int argc)
{
  (void)argc;
  if (MULTICORE_close()) {
    SET_TRUE_RETURN();
  } else {
    SET_FALSE_RETURN();
  }
}

void
mrbc_multicore_init(mrbc_vm *vm)
{
  mrbc_class *class_Multicore = mrbc_define_class(vm, "Multicore", mrbc_class_object);
  mrbc_define_method(vm, class_Multicore, "_open", c_multicore__open);
  mrbc_define_method(vm, class_Multicore, "_send", c_multicore__send);
  mrbc_define_method(vm, class_Multicore, "_take", c_multicore__take);
  mrbc_define_method(vm, class_Multicore, "_close", c_multicore__close);
}
