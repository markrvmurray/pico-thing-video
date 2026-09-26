# pico-thing-video

Firmware for an RP2350 (Pimoroni PGA2350) acting as a **slave bus peripheral**
on an MC6809E / HD6309 bus. The 6809 is the bus master and supplies the clocks;
PIO state machines and DMA watch the bus and answer in an 8-byte register
window at `$FF10`–`$FF17`.

Behind that window sit two devices the guest drives:

- **A DVI console and graphics engine** — 1024×480 RGB332 over HSTX, an 8×16
  text layer with CoCo3/OS-9 control codes and windows, and the CoCo3-style
  `$1B` graphics set (lines, fills, curves, flood fill, get/put buffers,
  patterns, mix modes).
- **USB host input** — a keyboard and mouse behind a hub, delivered as keyboard
  bytes and a polled mouse snapshot.

All of it is proven against a real 6809 running NitrOS-9, whose shell runs on
the DVI display from the USB keyboard.

## Hardware role

- **E and Q are supplied by the host** and sampled as inputs.
- **`!CS` is the attention trigger**: external glue decodes `$FF10`–`$FF17`,
  and the Pico sees only `!CS` and A2–A0, so the base address lives in the
  decoder.
- **`!RESET` is observed** (input, pulled up); the Pico clears its window.
- **`!IRQ` is a simulated open-collector output** (MC6850-shaped: asserted
  while receive interrupts are enabled and input is waiting).
- The firmware **follows the host's 5V rail**: it stands by until the rail is
  up, and reboots back into standby when it goes.

## Register window

MC6850-shaped; the guest reads it one byte at a time.

| Address | Read | Write |
|---|---|---|
| `$FF10` | next input byte (keyboard) | next command byte (console / graphics) |
| `$FF11` | status: RDRF, TDRE, READY, protocol version, MOUSE, IRQ | control: soft reset, loopback, receive-interrupt enable |
| `$FF12` | next byte of the 7-byte mouse snapshot | take a sample and restart the snapshot |
| `$FF13` | next byte of the 6-byte surface description | restart it |
| `$FF14`–`$FF17` | scratch | scratch |

The payload contract is `docs/copico-video-protocol.md` in the NitrOS-9 tree.
The status register's version field doubles as a liveness check: an absent or
resetting peripheral reads as the wrong version, and a guest must not trust any
other bit until it matches.

## Pin map

| GPIO | Signal | Dir | Notes |
|---|---|---|---|
| 0–1 | UART0 TX/RX | — | console, 115200 |
| 7 | `!FLT` | in | TPS2051B VBUS switch fault |
| 8 | USB role | out | CC mux + VBUS enable; high = host |
| 9 | HDMI HPD | in | hot-plug detect |
| 10–11 | HDMI DDC | io | I2C1, 100 kHz |
| 12–19 | HSTX DVI | out | 4 differential pairs |
| 20 | `!RESET` | in | observed |
| 21 | `!IRQ` | out | simulated open collector |
| 23–24 | bus trace | out | PIO side-set, debug |
| 25 / 26 | E / Q | in | supplied by host |
| 27 | `R/!W` | in | |
| 28–30 | A2–A0 | in | |
| 31–38 | D7–D0 | io | |
| 39 | `!CS` | in | attention trigger |
| 46 | ADC6 | in | host 5V rail via ÷2 |
| 47 | PSRAM CS | out | 8 MB PSRAM (XIP CS1) |

## Build

Requires the Pico SDK (as an environment variable or a CMake cache variable)
and Homebrew's bison 3.x on macOS (the system bison 2.3 is too old).

```bash
export PICO_SDK_PATH=<path-to-pico-sdk>
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release
cmake --build build-host -j
```

| CMake option | Default | Meaning |
|---|---|---|
| `USB_HOST_MODE` | ON | USB HID host; OFF builds a USB-CDC device instead |
| `FAST_SYS_CLK_320` | ON | 320 MHz system clock; OFF for 192 MHz |
| `SELFTEST` | ON | include the on-hardware self-tests (`se`) |
| `VIDEO_SCANOUT` | ON | OFF leaves the display dark (a measurement build) |

Both gcc and ATfE/clang are supported and produce different binaries; build
with both. `scripts/rebuild-all.sh` clean-builds every configuration and runs
the host tests (`tests/`) and the simulator suite.

The portable half of the firmware — the renderer, the guest protocol, the
console and the HID cooking — is compiled into the USim simulator
(`usim09pt --video`), whose test suite exercises it through the real register
protocol.

## Debugging

OpenOCD with `pico.cfg` (CMSIS-DAP over SWD); with two probes attached,
`probe_video.cfg` selects this board's by serial number. The console is on
UART0 in every build. `rp2350.svd` is included for register inspection.

## Interactive console

Every command has a two-letter alias.

| Command | Alias | Action |
|---|---|---|
| `help` | `h` | grouped help |
| `status [period]` | `st` | build, rail, temperature, bus and render timing, link and input counters, the register window; repeats every `period` ms |
| `trace [n]` | `tr` | the last `n` command bytes the renderer consumed, marked by role |
| `selftest` | `se` | on-hardware tests, with a canary that must fail (destructive) |
| `psram` | `ps` | time and verify PSRAM (destructive) |
| `priority` | `pr` | toggle scan-out DMA bus priority |
| `flood` | `fl` | feed the input FIFO continuously (measurement) |
| `verbose` | `vb` | echo cooked keyboard and mouse input (host builds) |
| `power` | `pw` | host rail and serving state |
| `i2c` | `ic` | HDMI hot-plug level and a DDC bus scan |
| `examine` | `ex` | dump the register window |
| `modify <off> <byte>...` | `mo` | write bytes into the window |
| `fill [value]` | `fi` | fill the window |
| `usb <args>` | `ub` | send to the USB-CDC guest console (device builds) |
