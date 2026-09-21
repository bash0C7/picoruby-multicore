/* Hand-written kernel table for the host tests. It speaks the same wire as the
 * table sendairk03 generates (one MessagePack array of arguments in, one
 * MessagePack value out, a status or a byte count returned), so the gem is
 * tested without suppify or spinel. Compiled into the host build only. */
#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <time.h>

#include "../../include/multicore.h"

/* ---- a small MessagePack reader / writer ---- */

static int32_t
be(const uint8_t *p, int n)
{
  int32_t v = 0;
  int i;
  for (i = 0; i < n; i++) {
    v = (v << 8) | p[i];
  }
  return v;
}

/* Bytes taken by the value at p, or -1. */
static int32_t
skip(const uint8_t *p, int32_t n)
{
  if (n < 1) {
    return -1;
  }
  uint8_t t = p[0];
  int32_t len = -1, count = -1, i, sub, off;
  int maps = 0;
  if (t <= 0x7f || t >= 0xe0 || t == 0xc0 || t == 0xc2 || t == 0xc3) {
    return 1;
  }
  if (t == 0xcc || t == 0xd0) return n >= 2 ? 2 : -1;
  if (t == 0xcd || t == 0xd1) return n >= 3 ? 3 : -1;
  if (t == 0xce || t == 0xd2 || t == 0xca) return n >= 5 ? 5 : -1;
  if (t == 0xcf || t == 0xd3 || t == 0xcb) return n >= 9 ? 9 : -1;
  if ((t & 0xe0) == 0xa0) len = 1 + (t & 0x1f);
  else if (t == 0xd9 || t == 0xc4) len = n >= 2 ? 2 + be(p + 1, 1) : -1;
  else if (t == 0xda || t == 0xc5) len = n >= 3 ? 3 + be(p + 1, 2) : -1;
  else if (t == 0xdb || t == 0xc6) len = n >= 5 ? 5 + be(p + 1, 4) : -1;
  if (len >= 0) {
    return len <= n ? len : -1;
  }
  if ((t & 0xf0) == 0x90) { count = t & 0x0f; off = 1; }
  else if (t == 0xdc) { if (n < 3) return -1; count = be(p + 1, 2); off = 3; }
  else if (t == 0xdd) { if (n < 5) return -1; count = be(p + 1, 4); off = 5; }
  else if ((t & 0xf0) == 0x80) { count = t & 0x0f; off = 1; maps = 1; }
  else if (t == 0xde) { if (n < 3) return -1; count = be(p + 1, 2); off = 3; maps = 1; }
  else if (t == 0xdf) { if (n < 5) return -1; count = be(p + 1, 4); off = 5; maps = 1; }
  else return -1;
  if (maps) count *= 2;
  for (i = 0; i < count; i++) {
    sub = skip(p + off, n - off);
    if (sub < 0) return -1;
    off += sub;
  }
  return off;
}

/* Array header at p: element count and header size, or false. */
static bool
arr_head(const uint8_t *p, int32_t n, int32_t *count, int32_t *off)
{
  if (n < 1) return false;
  if ((p[0] & 0xf0) == 0x90) { *count = p[0] & 0x0f; *off = 1; return true; }
  if (p[0] == 0xdc && n >= 3) { *count = be(p + 1, 2); *off = 3; return true; }
  if (p[0] == 0xdd && n >= 5) { *count = be(p + 1, 4); *off = 5; return true; }
  return false;
}

static bool
map_head(const uint8_t *p, int32_t n, int32_t *count, int32_t *off)
{
  if (n < 1) return false;
  if ((p[0] & 0xf0) == 0x80) { *count = p[0] & 0x0f; *off = 1; return true; }
  if (p[0] == 0xde && n >= 3) { *count = be(p + 1, 2); *off = 3; return true; }
  if (p[0] == 0xdf && n >= 5) { *count = be(p + 1, 4); *off = 5; return true; }
  return false;
}

static int32_t
read_int(const uint8_t *p, int32_t n, int64_t *v)
{
  uint8_t t;
  int64_t u = 0;
  int i, w;
  if (n < 1) return -1;
  t = p[0];
  if (t <= 0x7f) { *v = t; return 1; }
  if (t >= 0xe0) { *v = (int8_t)t; return 1; }
  if (t >= 0xcc && t <= 0xcf) {
    w = 1 << (t - 0xcc);
    if (n < 1 + w) return -1;
    for (i = 0; i < w; i++) u = (int64_t)(((uint64_t)u << 8) | p[1 + i]);
    *v = u;
    return 1 + w;
  }
  if (t >= 0xd0 && t <= 0xd3) {
    w = 1 << (t - 0xd0);
    if (n < 1 + w) return -1;
    for (i = 0; i < w; i++) u = (int64_t)(((uint64_t)u << 8) | p[1 + i]);
    if (w < 8 && (p[1] & 0x80)) u -= ((int64_t)1 << (8 * w));
    *v = u;
    return 1 + w;
  }
  return -1;
}

/* Writes v as the shortest signed form (fixint, int8 ... int64). */
static int32_t
write_int(int64_t v, uint8_t *out, int32_t cap)
{
  int w, i;
  if (v >= 0 && v <= 0x7f) { if (cap < 1) return -2; out[0] = (uint8_t)v; return 1; }
  if (v < 0 && v >= -32) { if (cap < 1) return -2; out[0] = (uint8_t)v; return 1; }
  if (v >= -0x80 && v <= 0x7f) w = 1;
  else if (v >= -0x8000 && v <= 0x7fff) w = 2;
  else if (v >= -0x80000000LL && v <= 0x7fffffffLL) w = 4;
  else w = 8;
  if (cap < 1 + w) return -2;
  out[0] = (uint8_t)(w == 1 ? 0xd0 : w == 2 ? 0xd1 : w == 4 ? 0xd2 : 0xd3);
  for (i = 0; i < w; i++) out[1 + i] = (uint8_t)((uint64_t)v >> (8 * (w - 1 - i)));
  return 1 + w;
}

static int32_t
write_array_head(int32_t count, uint8_t *out, int32_t cap)
{
  if (count < 16) { if (cap < 1) return -2; out[0] = (uint8_t)(0x90 | count); return 1; }
  if (cap < 3) return -2;
  out[0] = 0xdc; out[1] = (uint8_t)(count >> 8); out[2] = (uint8_t)count;
  return 3;
}

static int32_t
write_map_head(int32_t count, uint8_t *out, int32_t cap)
{
  if (count < 16) { if (cap < 1) return -2; out[0] = (uint8_t)(0x80 | count); return 1; }
  if (cap < 3) return -2;
  out[0] = 0xde; out[1] = (uint8_t)(count >> 8); out[2] = (uint8_t)count;
  return 3;
}

/* ---- the kernels ---- */

static int fake_init_calls;
static int64_t fake_stamp;

static void
fake_init(void)
{
  fake_init_calls++;
}

static const char *
fake_signature(void)
{
  return "(untyped) -> untyped";
}

/* echo([x]) -> x */
static int32_t
k_echo(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap)
{
  int32_t count, off, len;
  if (!arr_head(in, in_len, &count, &off) || count != 1) return -1;
  len = skip(in + off, in_len - off);
  if (len < 0) return -1;
  if (len > out_cap) return -2;
  memcpy(out, in + off, (size_t)len);
  return len;
}

/* add([a, b]) -> a + b */
static int32_t
k_add(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap)
{
  int32_t count, off, n;
  int64_t a, b;
  if (!arr_head(in, in_len, &count, &off) || count != 2) return -1;
  n = read_int(in + off, in_len - off, &a);
  if (n < 0) return -1;
  off += n;
  n = read_int(in + off, in_len - off, &b);
  if (n < 0) return -1;
  return write_int(a + b, out, out_cap);
}

/* scale_sum([xs, k]) -> sum of x * k */
static int32_t
k_scale_sum(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap)
{
  int32_t count, off, n, xc, xoff, i;
  int64_t k, x, total = 0;
  if (!arr_head(in, in_len, &count, &off) || count != 2) return -1;
  if (!arr_head(in + off, in_len - off, &xc, &xoff)) return -1;
  n = skip(in + off, in_len - off);
  if (n < 0) return -1;
  {
    int32_t korg = off + n;
    if (read_int(in + korg, in_len - korg, &k) < 0) return -1;
  }
  xoff += off;
  for (i = 0; i < xc; i++) {
    n = read_int(in + xoff, in_len - xoff, &x);
    if (n < 0) return -1;
    total += x * k;
    xoff += n;
  }
  return write_int(total, out, out_cap);
}

/* reverse([xs]) -> xs reversed */
static int32_t
k_reverse(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap)
{
  int32_t count, off, xc, xoff, i, n, w;
  int32_t starts[256], lens[256];
  if (!arr_head(in, in_len, &count, &off) || count != 1) return -1;
  if (!arr_head(in + off, in_len - off, &xc, &xoff) || xc > 256) return -1;
  xoff += off;
  for (i = 0; i < xc; i++) {
    n = skip(in + xoff, in_len - xoff);
    if (n < 0) return -1;
    starts[i] = xoff;
    lens[i] = n;
    xoff += n;
  }
  w = write_array_head(xc, out, out_cap);
  if (w < 0) return w;
  for (i = xc - 1; i >= 0; i--) {
    if (w + lens[i] > out_cap) return -2;
    memcpy(out + w, in + starts[i], (size_t)lens[i]);
    w += lens[i];
  }
  return w;
}

/* bump([h]) -> h with every Integer value + 1 (keys are copied as they are) */
static int32_t
k_bump(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap)
{
  int32_t count, off, mc, moff, i, n, w, m;
  int64_t v;
  if (!arr_head(in, in_len, &count, &off) || count != 1) return -1;
  if (!map_head(in + off, in_len - off, &mc, &moff)) return -1;
  moff += off;
  w = write_map_head(mc, out, out_cap);
  if (w < 0) return w;
  for (i = 0; i < mc; i++) {
    n = skip(in + moff, in_len - moff);
    if (n < 0) return -1;
    if (w + n > out_cap) return -2;
    memcpy(out + w, in + moff, (size_t)n);
    w += n;
    moff += n;
    n = read_int(in + moff, in_len - moff, &v);
    if (n < 0) return -1;
    moff += n;
    m = write_int(v + 1, out + w, out_cap - w);
    if (m < 0) return m;
    w += m;
  }
  return w;
}

/* dbl([xs]) -> every Float doubled, as float64 */
static int32_t
k_dbl(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap)
{
  int32_t count, off, xc, xoff, i, w;
  if (!arr_head(in, in_len, &count, &off) || count != 1) return -1;
  if (!arr_head(in + off, in_len - off, &xc, &xoff)) return -1;
  xoff += off;
  w = write_array_head(xc, out, out_cap);
  if (w < 0) return w;
  for (i = 0; i < xc; i++) {
    double d;
    uint64_t u = 0;
    int j;
    if (in_len - xoff < 9 || in[xoff] != 0xcb) return -1;
    for (j = 0; j < 8; j++) u = (u << 8) | in[xoff + 1 + j];
    memcpy(&d, &u, sizeof d);
    d *= 2.0;
    memcpy(&u, &d, sizeof u);
    if (w + 9 > out_cap) return -2;
    out[w] = 0xcb;
    for (j = 0; j < 8; j++) out[w + 1 + j] = (uint8_t)(u >> (8 * (7 - j)));
    w += 9;
    xoff += 9;
  }
  return w;
}

/* f32() -> 1.5 as float32 (0xca), which a kernel may answer with */
static int32_t
k_f32(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap)
{
  static const uint8_t v[5] = { 0xca, 0x3f, 0xc0, 0x00, 0x00 };
  (void)in; (void)in_len;
  if (out_cap < 5) return -2;
  memcpy(out, v, 5);
  return 5;
}

/* bigstr([n]) -> a String of n bytes ("x" repeated) */
static int32_t
k_bigstr(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap)
{
  int32_t count, off, hdr;
  int64_t n;
  if (!arr_head(in, in_len, &count, &off) || count != 1) return -1;
  if (read_int(in + off, in_len - off, &n) < 0 || n < 0) return -1;
  hdr = n < 32 ? 1 : n < 256 ? 2 : n < 65536 ? 3 : 5;
  if (hdr + n > out_cap) return -2;
  if (hdr == 1) out[0] = (uint8_t)(0xa0 | n);
  else if (hdr == 2) { out[0] = 0xd9; out[1] = (uint8_t)n; }
  else if (hdr == 3) { out[0] = 0xda; out[1] = (uint8_t)(n >> 8); out[2] = (uint8_t)n; }
  else { out[0] = 0xdb; out[1] = (uint8_t)(n >> 24); out[2] = (uint8_t)(n >> 16); out[3] = (uint8_t)(n >> 8); out[4] = (uint8_t)n; }
  memset(out + hdr, 'x', (size_t)n);
  return (int32_t)(hdr + n);
}

/* slow([ms]) -> nil, after sleeping ms milliseconds */
static int32_t
k_slow(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap)
{
  int32_t count, off;
  int64_t ms;
  struct timespec ts;
  if (!arr_head(in, in_len, &count, &off) || count != 1) return -1;
  if (read_int(in + off, in_len - off, &ms) < 0) return -1;
  ts.tv_sec = (time_t)(ms / 1000);
  ts.tv_nsec = (long)(ms % 1000) * 1000000L;
  nanosleep(&ts, NULL);
  if (out_cap < 1) return -2;
  out[0] = 0xc0;
  return 1;
}

/* stamp() -> 1, 2, 3 ... in the order the worker runs the calls */
static int32_t
k_stamp(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap)
{
  (void)in; (void)in_len;
  return write_int(++fake_stamp, out, out_cap);
}

/* init_calls() -> how many times a kernel's init has run */
static int32_t
k_init_calls(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap)
{
  (void)in; (void)in_len;
  return write_int(fake_init_calls, out, out_cap);
}

static int32_t k_boom(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap) { (void)in; (void)in_len; (void)out; (void)out_cap; return -3; }
static int32_t k_boom_silent(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap) { (void)in; (void)in_len; (void)out; (void)out_cap; return -3; }
static int32_t k_malformed(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap) { (void)in; (void)in_len; (void)out; (void)out_cap; return -1; }
static int32_t k_nospace(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap) { (void)in; (void)in_len; (void)out; (void)out_cap; return -2; }
static int32_t k_range(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap) { (void)in; (void)in_len; (void)out; (void)out_cap; return -4; }
static int32_t k_weird(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap) { (void)in; (void)in_len; (void)out; (void)out_cap; return -9; }

static const char *
fake_error_message(void)
{
  return "RuntimeError: bang";
}

/* Same bytes as :echo, different declared signatures: the Ruby layer turns the
 * wire's strs into Symbols where the return type says Symbol. */
#define SIGNED_ECHO(n, sig) \
  static const char *sig_##n(void) { return sig; } \
  static int32_t k_##n(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap) { return k_echo(in, in_len, out, out_cap); }
SIGNED_ECHO(sym, "(untyped) -> Symbol")
SIGNED_ECHO(syms, "(Array[Symbol], Hash[String, Integer]) -> Array[Symbol]")
SIGNED_ECHO(symhash, "(untyped) -> Hash[Symbol, Float]")
SIGNED_ECHO(symvals, "(untyped) -> Hash[String, Symbol]")
SIGNED_ECHO(symsym, "(untyped) -> Hash[Symbol, Symbol]")
SIGNED_ECHO(tuple, "(untyped) -> [Symbol, String, Integer]")
SIGNED_ECHO(optsym, "(untyped) -> Symbol?")
SIGNED_ECHO(nested, "(untyped) -> Array[Hash[Symbol, Array[Symbol]]]")
SIGNED_ECHO(strs, "(untyped) -> Array[String]")
SIGNED_ECHO(strhash, "(untyped) -> Hash[String, Integer]")
SIGNED_ECHO(symkeys_strvals, "(untyped) -> Hash[Symbol, String]")
SIGNED_ECHO(poly, "(untyped) -> untyped")

#define K(n) { #n, k_##n, fake_signature, fake_init, fake_error_message }
#define KS(n) { #n, k_##n, sig_##n, fake_init, fake_error_message }
const multicore_kernel_t multicore_kernels[] = {
  K(echo), K(add), K(scale_sum), K(reverse), K(bump), K(dbl), K(f32), K(bigstr),
  K(slow), K(stamp), K(init_calls), K(boom), K(malformed), K(nospace), K(range), K(weird),
  { "boom_silent", k_boom_silent, fake_signature, fake_init, NULL },
  KS(sym), KS(syms), KS(symhash), KS(symvals), KS(symsym), KS(tuple), KS(optsym), KS(nested),
  KS(strs), KS(strhash), KS(symkeys_strvals), KS(poly),
  { NULL, NULL, NULL, NULL, NULL }
};
