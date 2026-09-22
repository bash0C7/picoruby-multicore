# picoruby-multicore

Run a computation on another core from Ruby, by name.

You write the computation once as a kernel (a Ruby file with types, built
elsewhere into native code) and call it with `Multicore.run(:name, args...)`.
Arguments and results are ordinary Ruby values: Integer, Float, String, Symbol,
nil, true / false, and Arrays and Hashes of those, nested. The byte encoding,
the hand-over between the cores and the status codes are hidden; a failure is
an exception.

The Ruby VM stays on its own core. The kernel runs on the other one and never
touches the VM: there is no second VM instance and no interpreter over there
at all. A build compiles the kernel ahead of time with
[spinel](https://github.com/matz/spinel) into plain native code (wrapped into
a flat C ABI by [suppify](https://github.com/bash0C7/suppify)), and the other
core just calls that function directly. Writing the kernel in Ruby is for the
person, not the runtime: an ordinary Ruby method, typed once, checked against
a CRuby oracle before it ever reaches a board — not a second Ruby VM running
somewhere you can't see it.

> **Status.** The Ruby API and the C contract are settled. The `host` port is
> tested on both VMs. The `esp32` port (M5Stack Chain DualKey) and the
> `rp2040` port (Raspberry Pi Pico 2 W) are both verified on hardware: every
> argument/return type pattern, `spawn`, `open`, and every exception matches a
> CRuby oracle exactly.

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

**A build must provide `multicore_kernels`.** The gem has no default table. sendairk03 always emits
one (empty when there are no kernels), in which case every call raises
`Multicore::UnknownKernel`. A firmware link without it fails with
`undefined reference to `multicore_kernels'` (GNU ld) or
`Undefined symbols ... "_multicore_kernels"` (Apple ld), referenced from
`mc_find_kernel` and `MULTICORE_signature` in `ports/<board>/multicore.c`. Built alone (the host tests)
the table is a hand-written set of fake kernels in `test/support/fake_kernels.c`.

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

## Walkthrough: a kernel on ESP32 (Chain DualKey)

This walks through the whole path from a Ruby method to a value that came back from the other core,
on an ESP32 (M5Stack Chain DualKey, ESP-IDF).

### 1. Write the kernel

A kernel is a top-level Ruby method with its types spelled out as inline RBS on the line above it:

```ruby
# n-th Fibonacci number mod 1_000_000_007, so it stays inside the kernel's Integer width.
#: (Integer) -> Integer
def fib(n)
  a = 0
  b = 1
  i = 0
  while i < n
    t = (a + b) % 1_000_000_007
    a = b
    b = t
    i += 1
  end
  a
end
```

It is ordinary Ruby: run it under CRuby first and check the answer before it ever reaches a board.

### 2. Compile it into a `multicore_kernels` table

This gem does not compile Ruby itself; a build turns `.rb` kernel files into native code with
[spinel](https://github.com/matz/spinel) and wraps them into the `multicore_kernels` table (see
[The kernel table](#the-kernel-table) below) with [suppify](https://github.com/bash0C7/suppify). A
worked, tested setup for this step is `bash0C7/sendairk03`'s `docs/kernels.md`: drop the file under a
`kernels/` directory, and its `rake esp32:build` finds it, builds it, and links the generated gem in —
no separate command to remember.

### 3. Add this gem and its ESP32 port to the firmware

In the build configuration:

```ruby
conf.gem github: 'bash0C7/picoruby-multicore', branch: 'main'
```

In the ESP-IDF component's `CMakeLists.txt`, add `ports/esp32/multicore.c` to `idf_component_register`'s
`SRCS` (next to the other picoruby port sources it already lists):

```cmake
idf_component_register(
    SRCS
    # ...
    ${MULTICORE_GEM_DIR}/ports/esp32/multicore.c
    # ...
)
```

### 4. Call it from Ruby

```ruby
require "multicore"

puts "fib(40) => #{Multicore.run(:fib, 40).inspect}"
```

### 5. Flash it and read the answer

Flash the firmware and run the app. On Chain DualKey hardware this line prints:

```
fib(40) => 102334155
```

The worker started on core 1 for this call and freed its job slot afterward; `Multicore.run` blocked
the calling task until the value came back. Every kernel in this walkthrough's family (numeric, String,
Symbol, Array, Hash, `spawn`, `open`, and the `Multicore::KernelError` / `TypeError` / `RangeError`
paths) has been run this way on this hardware and every value matched a CRuby oracle exactly.

## Types

| Ruby | Wire | Comes back as |
|---|---|---|
| `nil`, `true`, `false` | nil, bool | the same |
| Integer | int (shortest signed form) | Integer |
| Float | float64 (always) | Float; a float32 reply is read too |
| String | str (raw bytes, UTF-8 or not, NUL allowed) | String |
| Symbol | str | Symbol where the kernel's return type says Symbol (see below), else String |
| Array | array | Array |
| Hash | map, in insertion order | Hash |

The wire has no Symbol, so a Symbol travels as a str. On the way back the Ruby layer reads the
**return type** of the called kernel's `signature()` (for example
`(Array[Symbol]) -> Hash[Symbol, Float]`) and turns Strings into Symbols exactly where the type says
`Symbol`: `Symbol`, `Array[Symbol]`, `Hash[Symbol, V]`, `Hash[K, Symbol]`, tuples, optionals, and nested
combinations of these. Every other String stays a String, and a signature it cannot read (or `untyped`)
leaves the result as the wire delivered it. Arguments need nothing: a Symbol argument already encodes as a str.

Anything else (a Range, your own class) raises `Multicore::TypeError` before
the worker is touched.

Floats keep every bit, including `-0.0`, infinities and subnormals. NaN keeps its NaN-ness but not its
payload: on mruby the VM itself stamps a serial number into the payload of each NaN it creates.

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
| `Multicore::NoMemory` | `malloc` could not supply the job slots (or core 1's stack on rp2040) when the worker started; nothing is left allocated |
| `Multicore::UnknownKernel` | the name is not in the table (the message lists the registered names) |
| `Multicore::QueueFull` | every job slot is taken |
| `Multicore::InputTooLarge` | the encoded arguments do not fit the input buffer |
| `Multicore::OutputTooLarge` | the result does not fit the output buffer |
| `Multicore::TypeError` | an argument is not the type in the kernel's signature, or a value cannot be sent |
| `Multicore::RangeError` | an Integer does not fit the kernel's Integer width |
| `Multicore::KernelError` | the kernel raised; the message carries the kernel's name and the message its `error_message` returns ("the kernel raised" when the entry has none) |
| `Multicore::Timeout` | no result within `timeout_ms` (the core stopped, the heap ran out and the kernel exited, ...), or `close` while a kernel is still running |

### Limits

- `Multicore.run` reads `timeout_ms` from a trailing `{ timeout_ms: n }` Hash (mruby/c cannot tell a Hash
  argument from keywords when a method also takes `*args`). A Hash argument whose only key is
  `:timeout_ms` is therefore read as the option and cannot be the last argument.
- A NaN result keeps its NaN-ness but not its payload bits (see Types).

## Build-time constants

Set them with `-D` where `ports/<board>/multicore.c` is compiled.

| Constant | Default | Meaning |
|---|---|---|
| `MULTICORE_IN_CAP` | 4096 | input buffer, bytes per job |
| `MULTICORE_OUT_CAP` | 4096 | output buffer, bytes per job |
| `MULTICORE_QUEUE_DEPTH` | 4 | job slots |
| `MULTICORE_STACK_BYTES` | 8192 | stack of the worker task / core 1 (rp2040: malloc'ed too) |

Nothing is reserved statically. The worker's start (the first `run` / `spawn` / `open`)
`malloc`s the job slots on the VM's core, and `close` frees them once the worker has really
stopped. A slot costs `MULTICORE_IN_CAP + MULTICORE_OUT_CAP + 28` bytes (8220 on the defaults), so the
heap holds `MULTICORE_QUEUE_DEPTH` times that while the worker runs (32,880 bytes on the defaults). On
rp2040 core 1's stack, `MULTICORE_STACK_BYTES` (8192, plus up to 8 bytes of alignment), is
`malloc`ed and freed the same way. The worker itself never allocates. When `close` cannot stop a
worker inside a long kernel within the bounded wait it raises `Multicore::Timeout` and everything
stays allocated until a later `close` (or the next start) finds the worker gone.

On esp32 the start logs (tag `multicore`) the heap headroom before it allocates, at INFO level: the
largest free block (`heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)`), the free heap and the minimum
free heap ever, next to the bytes the slots and the task stack need. When the start fails for lack of
memory it logs one line at error level naming what failed (the job slots, or the task stack) with the
requested bytes and the same three figures. The host and rp2040 ports log nothing.

`Multicore.in_cap`, `Multicore.out_cap` and `Multicore.queue_depth`
return the values a build was made with.

## Ports

| Port | Worker | Notes |
|---|---|---|
| `host` | a pthread | compiled into libmruby; runs the fake kernels; `MULTICORE_HOST_CORE_BUSY=1` in the environment makes the start report `CoreBusy`; `MULTICORE_HOST_FAIL_ALLOC=1` makes every allocation fail (`NoMemory`) |
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

`error_message` returns the message of the exception a call that answered `-3` raised. The worker calls
it after such a call and hands the text to Ruby as the `Multicore::KernelError` message.

## Layout

```
mrblib/multicore.rb            Multicore, Job, the MessagePack Encoder / Decoder, the signature walker, the exceptions
include/multicore.h            C contract (build constants, kernel table, submit / poll / result)
include/multicore_engine.h     job slots and the worker step, shared by every port
src/multicore.c                per-VM dispatch (src/mruby/, src/mrubyc/) and the Float <-> bytes helpers
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
the pthread port and the fake kernels. `test/c/run.sh` builds `test/c/engine_test.c` against the host port
and runs it under ThreadSanitizer and AddressSanitizer: start / close cycles, the allocation failure
path and close while a kernel runs, with the host port counting live allocations. `esp32` and `rp2040`
board behavior are both verified on hardware (see Status above).

## License

MIT
