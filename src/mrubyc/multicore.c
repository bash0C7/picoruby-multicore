/* mruby/c glue (included by ../multicore.c when PICORB_VM_MRUBYC). */
#include <mrubyc.h>

static void
c_multicore__start(mrbc_vm *vm, mrbc_value *v, int argc)
{
  SET_INT_RETURN(MULTICORE_start());
}

static void
c_multicore__stop(mrbc_vm *vm, mrbc_value *v, int argc)
{
  SET_BOOL_RETURN(MULTICORE_stop());
}

static void
c_multicore__running(mrbc_vm *vm, mrbc_value *v, int argc)
{
  SET_BOOL_RETURN(MULTICORE_running());
}

/* _submit(name, bytes) -> job id, or a negative MULTICORE_E_* */
static void
c_multicore__submit(mrbc_vm *vm, mrbc_value *v, int argc)
{
  char name_buf[MC_NAME_MAX];
  int name_len = mrbc_string_size(&v[1]);
  int in_len = mrbc_string_size(&v[2]);
  if (name_len >= MC_NAME_MAX) {
    SET_INT_RETURN(MULTICORE_E_UNKNOWN);
    return;
  }
  memcpy(name_buf, mrbc_string_cstr(&v[1]), (size_t)name_len);
  name_buf[name_len] = '\0';
  if (in_len > MULTICORE_IN_CAP) {
    SET_INT_RETURN(MULTICORE_E_INPUT_TOO_BIG);
    return;
  }
  SET_INT_RETURN(MULTICORE_submit(name_buf, (const uint8_t *)mrbc_string_cstr(&v[2]), (int32_t)in_len));
}

static void
c_multicore__poll(mrbc_vm *vm, mrbc_value *v, int argc)
{
  SET_INT_RETURN(MULTICORE_poll((int32_t)GET_INT_ARG(1)));
}

/* _result(job) -> [status, bytes] (and the job's slot is freed), or nil */
static void
c_multicore__result(mrbc_vm *vm, mrbc_value *v, int argc)
{
  int32_t job = (int32_t)GET_INT_ARG(1);
  const uint8_t *data;
  int32_t len, status;
  if (MULTICORE_result(job, &data, &len, &status) != 0) {
    SET_NIL_RETURN();
    return;
  }
  mrbc_value ary = mrbc_array_new(vm, 2);
  mrbc_value st = mrbc_integer_value(status);
  mrbc_value str = mrbc_string_new(vm, data, len);
  mrbc_array_push(&ary, &st);
  mrbc_array_push(&ary, &str);
  MULTICORE_release(job);
  SET_RETURN(ary);
}

static void
c_multicore__forget(mrbc_vm *vm, mrbc_value *v, int argc)
{
  MULTICORE_forget((int32_t)GET_INT_ARG(1));
  SET_NIL_RETURN();
}

/* _signature(name) -> the kernel's RBS signature, or nil when unregistered */
static void
c_multicore__signature(mrbc_vm *vm, mrbc_value *v, int argc)
{
  char name_buf[MC_NAME_MAX];
  int name_len = mrbc_string_size(&v[1]);
  if (name_len >= MC_NAME_MAX) {
    SET_NIL_RETURN();
    return;
  }
  memcpy(name_buf, mrbc_string_cstr(&v[1]), (size_t)name_len);
  name_buf[name_len] = '\0';
  const char *sig = MULTICORE_signature(name_buf);
  if (sig == NULL) {
    SET_NIL_RETURN();
  } else {
    SET_RETURN(mrbc_string_new_cstr(vm, sig));
  }
}

static void
c_multicore__names(mrbc_vm *vm, mrbc_value *v, int argc)
{
  char buf[MC_NAMES_MAX];
  MULTICORE_names(buf, (int)sizeof buf);
  SET_RETURN(mrbc_string_new_cstr(vm, buf));
}

/* _limits -> [input capacity, output capacity, queue depth] */
static void
c_multicore__limits(mrbc_vm *vm, mrbc_value *v, int argc)
{
  mrbc_value ary = mrbc_array_new(vm, 3);
  mrbc_value a = mrbc_integer_value(MULTICORE_in_cap());
  mrbc_value b = mrbc_integer_value(MULTICORE_out_cap());
  mrbc_value c = mrbc_integer_value(MULTICORE_queue_depth());
  mrbc_array_push(&ary, &a);
  mrbc_array_push(&ary, &b);
  mrbc_array_push(&ary, &c);
  SET_RETURN(ary);
}

static void
c_multicore__now_ms(mrbc_vm *vm, mrbc_value *v, int argc)
{
  SET_INT_RETURN((mrbc_int_t)MULTICORE_now_ms());
}

/* _f2s(float) -> the 8 bytes of its float64, big endian */
static void
c_multicore__f2s(mrbc_vm *vm, mrbc_value *v, int argc)
{
  uint8_t b[8];
  double d = v[1].tt == MRBC_TT_FLOAT ? (double)v[1].d : (double)v[1].i;
  mc_put_f64(d, b);
  SET_RETURN(mrbc_string_new(vm, b, 8));
}

/* _s2f(bytes) -> Float from 8 (float64) or 4 (float32) big endian bytes, nil otherwise */
static void
c_multicore__s2f(mrbc_vm *vm, mrbc_value *v, int argc)
{
  double d;
  if (!mc_get_float((const uint8_t *)mrbc_string_cstr(&v[1]), mrbc_string_size(&v[1]), &d)) {
    SET_NIL_RETURN();
    return;
  }
  SET_FLOAT_RETURN((mrbc_float_t)d);
}

void
mrbc_multicore_init(mrbc_vm *vm)
{
  mrbc_class *c = mrbc_define_class(vm, "Multicore", mrbc_class_object);
  mrbc_define_method(vm, c, "_start", c_multicore__start);
  mrbc_define_method(vm, c, "_stop", c_multicore__stop);
  mrbc_define_method(vm, c, "_running", c_multicore__running);
  mrbc_define_method(vm, c, "_submit", c_multicore__submit);
  mrbc_define_method(vm, c, "_poll", c_multicore__poll);
  mrbc_define_method(vm, c, "_result", c_multicore__result);
  mrbc_define_method(vm, c, "_forget", c_multicore__forget);
  mrbc_define_method(vm, c, "_signature", c_multicore__signature);
  mrbc_define_method(vm, c, "_names", c_multicore__names);
  mrbc_define_method(vm, c, "_limits", c_multicore__limits);
  mrbc_define_method(vm, c, "_now_ms", c_multicore__now_ms);
  mrbc_define_method(vm, c, "_f2s", c_multicore__f2s);
  mrbc_define_method(vm, c, "_s2f", c_multicore__s2f);
}
