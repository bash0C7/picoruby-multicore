/* mruby glue (included by ../multicore.c when PICORB_VM_MRUBY). */
#include <mruby.h>
#include <mruby/presym.h>
#include <mruby/class.h>
#include <mruby/array.h>
#include <mruby/string.h>

static mrb_value
mrb_multicore__start(mrb_state *mrb, mrb_value self)
{
  return mrb_fixnum_value(MULTICORE_start());
}

static mrb_value
mrb_multicore__stop(mrb_state *mrb, mrb_value self)
{
  return mrb_bool_value(MULTICORE_stop());
}

static mrb_value
mrb_multicore__running(mrb_state *mrb, mrb_value self)
{
  return mrb_bool_value(MULTICORE_running());
}

/* _submit(name, bytes) -> job id, or a negative MULTICORE_E_* */
static mrb_value
mrb_multicore__submit(mrb_state *mrb, mrb_value self)
{
  const char *name;
  const char *in;
  mrb_int name_len, in_len;
  char name_buf[MC_NAME_MAX];
  mrb_get_args(mrb, "ss", &name, &name_len, &in, &in_len);
  if (name_len >= MC_NAME_MAX) {
    return mrb_fixnum_value(MULTICORE_E_UNKNOWN);
  }
  memcpy(name_buf, name, (size_t)name_len);
  name_buf[name_len] = '\0';
  if (in_len > MULTICORE_IN_CAP) {
    return mrb_fixnum_value(MULTICORE_E_INPUT_TOO_BIG);
  }
  return mrb_fixnum_value(MULTICORE_submit(name_buf, (const uint8_t *)in, (int32_t)in_len));
}

static mrb_value
mrb_multicore__poll(mrb_state *mrb, mrb_value self)
{
  mrb_int job;
  mrb_get_args(mrb, "i", &job);
  return mrb_fixnum_value(MULTICORE_poll((int32_t)job));
}

/* _result(job) -> [status, bytes] (and the job's slot is freed), or nil */
static mrb_value
mrb_multicore__result(mrb_state *mrb, mrb_value self)
{
  mrb_int job;
  const uint8_t *data;
  int32_t len, status;
  mrb_get_args(mrb, "i", &job);
  if (MULTICORE_result((int32_t)job, &data, &len, &status) != 0) {
    return mrb_nil_value();
  }
  mrb_value ary = mrb_ary_new_capa(mrb, 2);
  mrb_ary_push(mrb, ary, mrb_fixnum_value(status));
  mrb_ary_push(mrb, ary, mrb_str_new(mrb, (const char *)data, len));
  MULTICORE_release((int32_t)job);
  return ary;
}

static mrb_value
mrb_multicore__forget(mrb_state *mrb, mrb_value self)
{
  mrb_int job;
  mrb_get_args(mrb, "i", &job);
  MULTICORE_forget((int32_t)job);
  return mrb_nil_value();
}

/* _signature(name) -> the kernel's RBS signature, or nil when unregistered */
static mrb_value
mrb_multicore__signature(mrb_state *mrb, mrb_value self)
{
  const char *name;
  mrb_int name_len;
  char name_buf[MC_NAME_MAX];
  mrb_get_args(mrb, "s", &name, &name_len);
  if (name_len >= MC_NAME_MAX) {
    return mrb_nil_value();
  }
  memcpy(name_buf, name, (size_t)name_len);
  name_buf[name_len] = '\0';
  const char *sig = MULTICORE_signature(name_buf);
  return sig == NULL ? mrb_nil_value() : mrb_str_new_cstr(mrb, sig);
}

static mrb_value
mrb_multicore__names(mrb_state *mrb, mrb_value self)
{
  char buf[MC_NAMES_MAX];
  MULTICORE_names(buf, (int)sizeof buf);
  return mrb_str_new_cstr(mrb, buf);
}

/* _limits -> [input capacity, output capacity, queue depth] */
static mrb_value
mrb_multicore__limits(mrb_state *mrb, mrb_value self)
{
  mrb_value ary = mrb_ary_new_capa(mrb, 3);
  mrb_ary_push(mrb, ary, mrb_fixnum_value(MULTICORE_in_cap()));
  mrb_ary_push(mrb, ary, mrb_fixnum_value(MULTICORE_out_cap()));
  mrb_ary_push(mrb, ary, mrb_fixnum_value(MULTICORE_queue_depth()));
  return ary;
}

static mrb_value
mrb_multicore__now_ms(mrb_state *mrb, mrb_value self)
{
  return mrb_fixnum_value((mrb_int)MULTICORE_now_ms());
}

/* _f2s(float) -> the 8 bytes of its float64, big endian */
static mrb_value
mrb_multicore__f2s(mrb_state *mrb, mrb_value self)
{
  mrb_float f;
  uint8_t b[8];
  mrb_get_args(mrb, "f", &f);
  mc_put_f64((double)f, b);
  return mrb_str_new(mrb, (const char *)b, 8);
}

/* _s2f(bytes) -> Float from 8 (float64) or 4 (float32) big endian bytes, nil otherwise */
static mrb_value
mrb_multicore__s2f(mrb_state *mrb, mrb_value self)
{
  const char *p;
  mrb_int len;
  double d;
  mrb_get_args(mrb, "s", &p, &len);
  if (!mc_get_float((const uint8_t *)p, (int)len, &d)) {
    return mrb_nil_value();
  }
  return mrb_float_value(mrb, (mrb_float)d);
}

void
mrb_picoruby_multicore_gem_init(mrb_state *mrb)
{
  /* gem init runs before mrblib is evaluated, so the class is defined here
   * (mrb_class_get_id would not find it yet) and mrblib reopens it. */
  struct RClass *c = mrb_define_class_id(mrb, MRB_SYM(Multicore), mrb->object_class);
  mrb_define_method_id(mrb, c, MRB_SYM(_start), mrb_multicore__start, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, c, MRB_SYM(_stop), mrb_multicore__stop, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, c, MRB_SYM(_running), mrb_multicore__running, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, c, MRB_SYM(_submit), mrb_multicore__submit, MRB_ARGS_REQ(2));
  mrb_define_method_id(mrb, c, MRB_SYM(_poll), mrb_multicore__poll, MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, c, MRB_SYM(_result), mrb_multicore__result, MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, c, MRB_SYM(_forget), mrb_multicore__forget, MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, c, MRB_SYM(_signature), mrb_multicore__signature, MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, c, MRB_SYM(_names), mrb_multicore__names, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, c, MRB_SYM(_limits), mrb_multicore__limits, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, c, MRB_SYM(_now_ms), mrb_multicore__now_ms, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, c, MRB_SYM(_f2s), mrb_multicore__f2s, MRB_ARGS_REQ(1));
  mrb_define_method_id(mrb, c, MRB_SYM(_s2f), mrb_multicore__s2f, MRB_ARGS_REQ(1));
}

void
mrb_picoruby_multicore_gem_final(mrb_state *mrb)
{
  (void)mrb;
  MULTICORE_stop();
}
