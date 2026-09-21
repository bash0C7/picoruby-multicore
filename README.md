# picoruby-multicore

Run a computation on another core from Ruby, by name.

You write the computation once as a kernel (a Ruby file with types, built
elsewhere into native code) and call it with `Multicore.run(:name, args...)`.
Arguments and results are ordinary Ruby values: Integer, Float, String, Symbol,
nil, true / false, and Arrays and Hashes of those, nested. The byte encoding,
the hand-over between the cores and the status codes are hidden; a failure is
an exception.

The Ruby VM stays on its own core. The kernel runs on the other one and never
touches the VM.

> **Status: DRAFT.** The Ruby API and the C contract are settled. The `host`
> port is tested on both VMs; the `esp32` and `rp2040` ports have been compiled
> for syntax only and are not yet verified on a board.

## Installation

Add this line to your PicoRuby build configuration:

```ruby
conf.gem github: 'bash0C7/picoruby-multicore', branch: 'main'
# or, during local bring-up:
conf.gem gemdir: '/path/to/picoruby-multicore'
```

The ports compile with a board SDK's include paths, so a firmware build adds
`ports/<board>/multicore.c` to its own source list (ESP-IDF `SRCS`, the CMake
source list on RP2350) rather than to libmruby.

## Dependencies

None besides PicoRuby itself. Works on both mruby and mruby/c builds.

## Usage

```ruby
require 'multicore'

Multicore.run(:scale_sum, [1, 2, 3], 10)             # => 60

job = Multicore.spawn(:scale_sum, [1, 2, 3], 10)     # returns at once
job.done?                                            # never blocks
job.value                                            # => 60 (waits if needed)

Multicore.open { |mc| mc.run(:dbl, [1.5]) }          # => [3.0]; the core is released on the way out
Multicore.close                                      # stop the worker explicitly
```

The worker starts by itself on the first `run` or `spawn`. If something else is
using the second core, that call raises `Multicore::CoreBusy`.

### Where the kernels come from

`:scale_sum`, `:dbl` and the other names are kernels a build registers in a
table (`multicore_kernels`, see `include/multicore.h`). A build such as
sendairk03 generates the table from `kernels/*.rb`, where each top-level `def`
carries its types as inline RBS. This gem only calls what is in the table.

Built alone (the host tests) the table is a hand-written set of fake kernels in
`test/support/fake_kernels.c`. A firmware build with no kernels links against an
empty table and every call raises `Multicore::UnknownKernel`.

### run, spawn, value

- `Multicore.run(name, *args, timeout_ms: 10_000)` calls the kernel and returns
  its result. `timeout_ms: nil` waits without limit.
- `Multicore.spawn(name, *args)` queues the call and returns a `Multicore::Job`.
  - `job.done?` is non-blocking.
  - `job.value(timeout_ms: 10_000)` and `job.wait(timeout_ms: 10_000)` wait for
    the kernel. On `Multicore::Timeout` the job goes on and can be waited for
    again.
  - `job.discard` drops a job whose result you no longer want.
- The worker is one core, so spawned jobs run one after another in the order
  they were queued. Each result belongs to its job, so you can collect them in
  any order.
- A job holds its queue slot until you collect it (`done?` or `value`). When
  every slot is taken `spawn` raises `Multicore::QueueFull`.

`run` reads `timeout_ms` from a trailing `{ timeout_ms: n }` Hash. A Hash
argument whose only key is `:timeout_ms` therefore cannot be the last argument.

## Types

| Ruby | Wire | Comes back as |
|---|---|---|
| `nil`, `true`, `false` | nil, bool | the same |
| Integer | int (shortest signed form) | Integer |
| Float | float64 (always) | Float; a float32 reply is read too |
| String | str (raw bytes, UTF-8 or not, NUL allowed) | String |
| Symbol | str | **String** (the wire has no Symbol) |
| Array | array | Array |
| Hash | map, in insertion order | Hash (keys as they are on the wire: Strings) |

Anything else (a Range, your own class) raises `Multicore::TypeError` before
the worker is touched.

Floats keep every bit, including `-0.0`, infinities, subnormals and NaN. On
mruby the VM itself stamps a serial number into the payload of each NaN it
creates, so a NaN keeps its NaN-ness but not its payload.

### Integer width

The kernel's Integer is the width spinel gives it: 8 bytes on a host, 4 bytes
on an MCU. The VMs' Integer is 64 bits wide on every target. The Ruby side never
narrows or checks a value: the kernel answers status `-4` for an Integer that
does not fit its width, and that becomes `Multicore::RangeError`. A kernel's
result is always in the kernel's range, so it fits.

## Exceptions

All are under `Multicore::Error < StandardError`. A numeric status never
reaches Ruby.

| Exception | Raised when |
|---|---|
| `Multicore::CoreBusy` | the second core is in use by something else |
| `Multicore::UnknownKernel` | the name is not in the table (the message lists the registered names) |
| `Multicore::QueueFull` | every job slot is taken |
| `Multicore::InputTooLarge` | the encoded arguments do not fit the input buffer |
| `Multicore::OutputTooLarge` | the result does not fit the output buffer |
| `Multicore::TypeError` | an argument is not the type in the kernel's signature, or a value cannot be sent |
| `Multicore::RangeError` | an Integer does not fit the kernel's Integer width |
| `Multicore::KernelError` | the kernel raised; the message carries its exception class and message |
| `Multicore::Timeout` | no result within `timeout_ms` (the core stopped, the heap ran out and the kernel exited, ...), or `close` while a kernel is still running |

## Build-time constants

Set them with `-D` where `ports/<board>/multicore.c` is compiled.

| Constant | Default | Meaning |
|---|---|---|
| `MULTICORE_IN_CAP` | 4096 | input buffer, bytes per job |
| `MULTICORE_OUT_CAP` | 4096 | output buffer, bytes per job |
| `MULTICORE_QUEUE_DEPTH` | 8 | job slots |
| `MULTICORE_STACK_BYTES` | 8192 | stack of the worker task / core 1 |

RAM used is `MULTICORE_QUEUE_DEPTH * (MULTICORE_IN_CAP + MULTICORE_OUT_CAP)`
bytes. `Multicore.in_cap`, `Multicore.out_cap` and `Multicore.queue_depth`
return the values a build was made with.

## Ports

| Port | Worker | Notes |
|---|---|---|
| `host` | a pthread | compiled into libmruby; runs the fake kernels; `MULTICORE_HOST_CORE_BUSY=1` in the environment makes the start report `CoreBusy` |
| `esp32` | a FreeRTOS task pinned to core 1 | woken with a task notification |
| `rp2040` | pico-sdk core 1 | RAM resident loop and dispatch (`__not_in_flash_func`); shared-RAM state flags and `__dmb()`; `__sev()` / `__wfe()` as the doorbell |

### RP2040 notes

- Core 1 is also used by `picoruby-psg`. Only one of them can own it:
  `Multicore.run` / `Multicore.open` raise `Multicore::CoreBusy` while core 1 is
  taken.
- The hardware FIFO is not used to carry anything and none of its words reaches
  Ruby.
- The worker loop and the dispatch run from RAM. The kernel itself is ordinary
  code, so keep a kernel that must not stall on flash access short, and read the
  BOOTSEL button from core 0 only.

## The kernel table

`include/multicore.h` declares the table a build provides:

```c
typedef struct {
  const char *name;
  int32_t (*call)(const uint8_t *in, int32_t in_len, uint8_t *out, int32_t out_cap);
  const char *(*signature)(void);
  void (*init)(void);
} multicore_kernel_t;
extern const multicore_kernel_t multicore_kernels[];   /* last entry: all NULL */
```

`call` takes one MessagePack array (one element per argument) and writes one
MessagePack value. It returns the bytes written, or `-1` (arguments do not fit
the signature), `-2` (output buffer too small), `-3` (the kernel raised) or `-4`
(Integer out of range). `init` runs once before that kernel's first call.

`src/kernel_table.c` supplies a weak empty table and a weak
`multicore_kernel_error_message(const multicore_kernel_t *)` that returns NULL.
A build that can name a library's error message overrides the second so a
`Multicore::KernelError` carries it. The strong table has to be linked in: when
it lives in a static archive that also holds the weak default, make sure the
strong definition is the one that gets pulled.

## Layout

```
mrblib/multicore.rb            Multicore, Job, the MessagePack Encoder / Decoder, the exceptions
include/multicore.h            C contract (build constants, kernel table, submit / poll / result)
include/multicore_engine.h     job slots and the worker step, shared by every port
src/multicore.c                per-VM dispatch (src/mruby/, src/mrubyc/) and the Float <-> bytes helpers
src/kernel_table.c             weak empty kernel table
ports/host/                    pthread worker
ports/esp32/                   FreeRTOS task on core 1
ports/rp2040/                  pico-sdk core 1
test/support/fake_kernels.c    the hand-written table the host tests run against
```

The C side never raises: it returns a status, a String or nil, and the Ruby
layer turns that into values and exceptions, so both VMs share one Ruby layer.
The Ruby layer stays inside the mruby/c subset.

## Development

The host tests run through the PicoRuby harness (picotest) on both VMs, with
the pthread port and the fake kernels. Board behavior is verified on hardware.

## License

MIT
