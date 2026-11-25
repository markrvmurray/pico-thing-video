# pico-thing-video

This is the firmware for the Pico-Thing's video and input processor. It runs on
an RP2350 (a Pimoroni PGA2350), which acts as a **slave bus peripheral** on an
MC6809E / HD6309 bus. The 6809 is the bus master and supplies the clocks. PIO
state machines and DMA watch the bus and answer in an 8-byte register window at
`$FF10`–`$FF17`.

Behind that window, the guest drives three devices:

- **A DVI console and graphics engine.** It outputs 1024×480 RGB332 over HSTX.
  Text is an 8×16 layer that follows the CoCo 3 / OS-9 control codes. It
  supports windows and overlay windows, and each window has its own colours.
  Graphics use the full CoCo 3 `$1B` set (see below).
- **USB host input.** A keyboard and mouse sit behind a hub. Keys arrive as
  bytes, and the mouse is read as a polled snapshot.
- **A tone generator** on a PWM pin, used for the bell and for `SS.Tone`.

The bus, video, USB host and PSRAM have all been tested against a real 6809 on
the v2.0 board. That 6809 runs NitrOS-9, with its shell on the DVI display and
input from the USB keyboard.

Related repositories:

- [pico-thing-main](https://github.com/markrvmurray/pico-thing-main): the main
  board's companion firmware (RAM, DAT, ACIAs).
- [usim, branch `pico-thing`](https://github.com/markrvmurray/usim/tree/pico-thing):
  the simulator, which compiles this renderer and tests it.
- [nitros9](https://github.com/markrvmurray/nitros9): the guest. Its
  `docs/copico-video-protocol.md` is the contract for this window.

## Status

| Area | State |
|---|---|
| Bus window, `!IRQ`, `!RESET`, power-follow | Working on the board |
| DVI text, windows, the full `$1B` graphics set | Working on the board |
| USB keyboard and mouse, auto-repeat | Working on the board |
| PSRAM-backed stores (graphics shadow, buffers, parked windows, trace) | Working on the board |
| Single-byte key codes, per-window text colours, overlay windows, VT100 size queries | Tested in usim; not yet run on the board |

## Hardware role

- **E and Q are supplied by the host**, and the Pico samples them as inputs.
- **`!CS` is the attention trigger.** External glue logic decodes
  `$FF10`–`$FF17`, and the Pico sees only `!CS` and A2–A0. The base address
  therefore lives in the decoder.
- **`!RESET` is watched** (an input, pulled up). When it is asserted, the Pico
  clears its window.
- **`!IRQ` is a simulated open-collector output**, behaving like an MC6850's:
  it is asserted while receive interrupts are enabled and input is waiting.
- **The firmware follows the host's 5V rail.** It stands by until the rail
  comes up, and reboots back into standby when the rail goes down.

## Register window

The window is shaped like an MC6850. **The guest must read it one byte at a
time**, because reads have side effects. It must also check TDRE before
writing every command byte, because a byte written into a full FIFO is
dropped.

| Address | Read | Write |
|---|---|---|
| `$FF10` | next input byte (keyboard) | next command byte (console / graphics) |
| `$FF11` | status: RDRF, TDRE, READY, protocol version, MOUSE, IRQ | control: soft reset, loopback, receive-interrupt enable |
| `$FF12` | next byte of the 7-byte mouse snapshot | take a sample and restart the snapshot |
| `$FF13` | next byte of the 6-byte description of the current window | restart it |
| `$FF14`–`$FF17` | scratch | scratch |

The status register's version field is also a liveness check. An absent or
resetting peripheral reads as the wrong version (`$FF` or `$00`), so a guest
must not trust any other status bit until the version matches.

### Command stream

The guest writes command bytes to `$FF10`:

- **Text:** the CoCo 3 control codes (cursor movement, erase, clear, bell, wrap
  and scroll). Also supported:
  - the `$1F` family: inverse video, and insert/delete line;
  - `ESC [` cursor positioning and the VT100 size and position queries, so a
    serial guest can learn the screen size;
  - ESC 7 / ESC 8 to save and restore the cursor.
- **Windows:** DWSet, Select, DWShow, DWEnd, CWArea, DWProtSw, and overlay
  windows (OWSet / OWEnd). Each window has its own FColor, BColor and
  inverse settings. Windows that are not on screen are stored in PSRAM.
  Select (where text goes) and DWShow (what is displayed) are separate
  commands, so a background shell can write to its window without taking over
  the display.
- **Graphics (`$1B`):**
  - points, lines, boxes and bars;
  - circles, ellipses and arcs, following GrfDrv's geometry;
  - flood fill;
  - get/put buffers in a 2 MB PSRAM pool;
  - fonts, PSet patterns, and the LSet mix modes;
  - palette and default palette.

  A flood fill is drawn in slices across render passes, so no single pass
  runs long.
- **`$1B $27` Tone** (a Pico-Thing addition): takes CoCo 3 `SS.Tone` values
  for pitch, duration and volume.

### Keyboard bytes

The keyboard uses a UK layout. Every key produces exactly one byte:

| Key | Byte |
|---|---|
| F1–F12 | `$80`–`$8B` |
| Arrows (up, down, left, right) | `$8C`–`$8F` |
| Shift + arrows | `$90`–`$93` |
| Ctrl + arrows | `$94`–`$97` |
| Home, End, PgUp, PgDn | `$98`–`$9B` |
| Ins, Del (forward) | `$9D`, `$9E` |
| £ | `$9C` |
| Alt + a printable character | that character + `$80` |

The firmware generates key repeat itself: after 500 ms, then 20 per second. A
repeat is never queued behind a byte the guest hasn't read yet.

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
| 40 | sound | out | PWM tone to an external filter and amplifier |
| 46 | ADC6 | in | host 5V rail via ÷2 |
| 47 | PSRAM CS | out | 8 MB PSRAM (XIP CS1) |

## Build

You need:

- the Pico SDK, given either as an environment variable or as a CMake cache
  variable;
- flex, and bison 3.x (on macOS, use Homebrew's bison, because the system's
  2.3 is too old).

The board is set to `pimoroni_pga2350` in `CMakeLists.txt`.

```bash
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release \
      -DPICO_SDK_PATH=<path-to-pico-sdk>
cmake --build build-host -j
```

To build with ATfE (clang) instead of gcc, add these options:

```bash
-DPICO_TOOLCHAIN_PATH=<path-to-ATfE> -DPICO_COMPILER=pico_arm_cortex_m33_clang
```

| CMake option | Default | Meaning |
|---|---|---|
| `USB_HOST_MODE` | ON | USB HID host; OFF builds a USB-CDC device instead |
| `FAST_SYS_CLK_320` | ON | 320 MHz system clock; OFF for 192 MHz |
| `SELFTEST` | ON | include the on-hardware self-tests (`se`) |
| `VIDEO_SCANOUT` | ON | OFF leaves the display dark (a measurement build) |

These are CMake `option()`s. Changing a default does not change a tree that is
already configured; use `cmake -U <OPTION>` for that.

**Build with both gcc and ATfE before trusting a result.** The two compilers
produce genuinely different binaries.

## Testing

- **`scripts/rebuild-all.sh`** clean-rebuilds every configured build tree. It
  then runs the host tests and the usim suite. It fails if any tree it expects
  is missing.
- **Host tests** (`tests/`, Catch2): the console parser, the ADC and tone
  conversions, and the `mem*` routines. CI runs the parser tests on every push.
- **usim suite:** the renderer, guest protocol, console and HID cooking contain
  no Pico-specific code. usim compiles them unchanged (`usim09pt --video`) and
  tests them through the real register protocol, with an emulated 6809 behind
  them.
- **On-hardware self-test** (`selftest`): checks the PSRAM layout and retention,
  and times the drawing paths. It includes a canary check that must fail; if
  the canary ever passes, the rest of the report means nothing.

## Flashing and debugging

OpenOCD uses `pico.cfg`, the only config in the repo (CMSIS-DAP over SWD). It
is pinned to this board's probe by serial number, so it never picks the main
board's probe. It is self-contained, so CLion, `scripts/flash.sh` and a plain
`openocd -f pico.cfg` can all use it.

```bash
scripts/flash.sh [BUILD_DIR] [flash|noreset|reset]
```

`BUILD_DIR` defaults to `cmake-build-relwithdebinfo-atfe-23`. The script
flashes an existing build and never builds anything itself.

The console is on UART0 in every build. To inspect registers, load the Pico
SDK's `src/rp2350/hardware_regs/RP2350.svd` into the debugger.

## Interactive console

Every command has a two-letter alias.

| Command | Alias | Action |
|---|---|---|
| `help` | `h` | grouped help |
| `status [period]` | `st` | build identity, rail, temperature, bus and render timing, link and input counters; repeats every `period` ms |
| `trace [n]` | `tr` | the last `n` command bytes consumed, marked by role |
| `selftest` | `se` | on-hardware tests, with a canary that must fail (destructive) |
| `psram` | `ps` | time and verify PSRAM (destructive) |
| `priority` | `pr` | toggle scan-out DMA bus priority |
| `flood` | `fl` | feed the input FIFO continuously (measurement) |
| `verbose` | `vb` | echo cooked keyboard and mouse input (host builds) |
| `power` | `pw` | host rail and serving state |
| `beep [hz [ms]]` | `bp` | play a tone (default: the bell) |
| `i2c` | `ic` | HDMI hot-plug level and a DDC bus scan |
| `examine` | `ex` | dump the register window |
| `modify <off> <byte>...` | `mo` | write bytes into the window |
| `fill [value]` | `fi` | fill the window |
| `usb <args>` | `ub` | send to the USB-CDC guest console (device builds) |

## Licence

MIT; see `LICENSE`.
