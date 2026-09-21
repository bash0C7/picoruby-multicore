# picoruby-multicore

A multicore worker binding for PicoRuby. Run a native C kernel on another core
and exchange scalars with your Ruby code, without blocking the VM.

The Ruby VM stays on core 0. The worker never touches the VM: Ruby sends an
integer, the worker computes on the other core, and Ruby takes the result when it
is ready. Fan out with several `send` calls and join with a `take` loop.

> **Status: DRAFT.** The interface (`mrblib` / `include`) is settled. The board
> ports are being brought up: `host`, `esp32` (ESP32-S3) and `rp2040` (Pico 2 W).

## Installation

Add this line to your PicoRuby build configuration:

```ruby
conf.gem github: 'bash0C7/picoruby-multicore', branch: 'main'
# or, during local bring-up:
conf.gem gemdir: '/path/to/picoruby-multicore'
```

The board port that compiles is chosen by the target. Firmware builds that do not
pick up a gem's `ports/<board>/*.c` by themselves need that C source added to the
firmware build definition (ESP-IDF `SRCS`, or the CMake source list on RP2350).

## Dependencies

None besides PicoRuby itself. Works on both mruby and mruby/c builds.

## Quick Start

```ruby
require 'multicore'

mc = Multicore.new(unit: :core1_lcg)

mc.send(300_000)            # returns at once; the worker runs on core 1
result = mc.take            # nil until the result is ready
result = mc.take until result

mc.close
```

`send` accepts an Integer. `take` returns the result Integer, or `nil` when
nothing is ready. Both never block.

## Units

`unit:` names a worker the port provides, in the same way `I2C.new(unit:)` names a
bus. The Ruby side keeps no table of names: it passes the name to the C port, which
maps it to its worker. An unknown unit raises `Multicore::Error`.

| Port | Unit | Worker | Runs on |
|---|---|---|---|
| `esp32` | `:core1_lcg` | LCG kernel (32-bit linear congruential generator, n iterations) | core 1 (FreeRTOS task pinned) |
| `rp2040` | `:core1_lcg` | the same LCG kernel | core 1 (RAM resident) |
| `host` | `:host_lcg` | the same LCG kernel, run on the calling thread | the calling thread |

The same input gives the same result on every port.

### RP2040 notes

- Core 1 is also used by `picoruby-psg`. Only one of them can own core 1:
  `Multicore.new` raises `Multicore::Error` when core 1 is already taken.
- The worker code is RAM resident. It never calls into the BLE stack, lwIP, or
  flash. Read the BOOTSEL button from core 0 only.

## Layout

```
mrblib/multicore.rb     Multicore, Multicore::Error, scalar check (shared Ruby)
include/multicore.h     C contract shared by all ports
src/                    per-VM bindings (mruby, mruby/c)
ports/host/             runs the kernel on the calling thread
ports/esp32/            FreeRTOS task and queue on core 1
ports/rp2040/           pico-sdk multicore on core 1
```

## Error handling

`Multicore::Error` is raised for an unknown unit, an unavailable core, a value that
is not an Integer, or a call on a closed worker.

## Development

Host-side tests run the `host` port on both VMs through the PicoRuby harness.
Board behavior is verified on hardware: a worker started from a Ruby loop must
return the same result as the host and must not stall the Ruby side.

## License

MIT
