# pico-thing-video

Firmware for a Raspberry Pi Pico2 (RP2350 / PGA2350) acting as a **slave bus
peripheral** on an external Motorola MC6809E microprocessor's bus. The 6809 is
the bus master and drives the clocks; the Pico2 watches the address/data bus via
PIO state machines and responds when selected, presenting an 8-byte register
window that the 6809 can read and write.

A layered HSTX→DVI video engine is now integrated (see [Video](#video)); a
USB-host keyboard/mouse is still planned.

## Hardware role

- **E and Q clocks are supplied by the host** and sampled as inputs — the Pico
  does not generate them.
- **`!CS` is the attention trigger.** When the host asserts chip-select, the
  Pico services the cycle; there is no VMA/run-state gating.
- **`!RESET` is observed** (input, pulled up): the host drives it, the Pico
  notices and clears its register window.
- **`!IRQ` is a simulated open-collector output** back to the host: driving the
  pin as an output asserts (low), switching it to an input releases it (hi-Z).

## Register window

The Pico exposes an **8-byte register window at guest `$FF10`–`$FF17`** (see
`include/registers.h`). External address-decode glue asserts `!CS` over that
range; the Pico sees only `!CS` and A2–A0, so the base address lives in the
decoder, not the firmware. Reads and writes to the same offset can map to
separate host-side buffers (`read_registers[]` / `write_registers[]`), allowing
read-only enforcement. Device sub-registers (video, keyboard, mouse) will be
laid out in this window as they are added.

## Video

A layered HSTX→DVI text+graphics engine (spliced from `../pico-vidterm`) runs on
Core 1:

- **Output:** DVI on **GP12–19** (four differential pairs, Pico DVI Sock wiring)
  — 1024×480, RGB332, ~60 Hz. Needs an RP2350 with an HDMI/DVI breakout.
- **Framebuffer** in SRAM, scanned out by a ping-pong DMA pair on a per-scanline
  IRQ (its own `DMA_IRQ_2`, at higher priority than the bus). An 8×16 Spleen-font
  text layer is composited on top.
- A built-in **test screen** (compiled in via `VIDEO_DEMO`, default on) draws a
  border, ruler, the full character set, and diagonals at startup.

The `$FF10` register interface for the 6809 to drive the video isn't wired up
yet — the engine and its test pattern are the current state. See `src/video.cpp`.

## Pin map

The timing-critical bus pins are owned by the PIO programs. The bus block spans
GPIO 23–39 (above the default 0–31 window), so each bus PIO sets its GPIO base to
16 before initialising pins. `!CS` sits above the data bus so the write state
machine can read `R/!W`, the address, and the data as one contiguous field.

| GPIO  | Signal      | Dir | Notes                          |
|-------|-------------|-----|--------------------------------|
| 0–1   | UART TX/RX  | —   | console (UART0)                |
| 12–19 | HSTX DVI    | out | 4 differential pairs (DVI Sock)|
| 20    | `!RESET`    | in  | host reset, observed           |
| 21    | `!IRQ`      | out | simulated open-collector       |
| 23–24 | Bus trace   | out | PIO side-set, debug            |
| 25    | E clock     | in  | supplied by host               |
| 26    | Q clock     | in  | supplied by host               |
| 27    | `R/!W`      | in  |                                |
| 28–30 | A2–A0       | in  | address bus                    |
| 31–38 | D7–D0       | io  | data bus                       |
| 39    | `!CS`       | in  | attention trigger              |

## Build

Requires the Pico SDK. `PICO_SDK_PATH` may be supplied either as an environment
variable or as a CMake cache variable (`-DPICO_SDK_PATH=...`); both work.

```bash
export PICO_SDK_PATH=<path-to-pico-sdk>
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Outputs: `build/pico-thing-video.uf2` (flash via USB mass storage) and
`build/pico-thing-video.elf` (for the debugger).

The build also runs flex/bison on the host to generate the console command
parser, and auto-generates a PIO header from `src/pico_thing_video.pio`.
`compile_commands.json` is emitted on every configure for clangd / CLion.

macOS ships bison 2.3, which is too old — install Homebrew's bison 3.x with
`brew install bison`; the build picks it up automatically.

### CLion

Open the project in CLion and add `-DPICO_SDK_PATH=<path-to-sdk>` to the CMake
profile's CMake options (Settings → Build, Execution, Deployment → CMake). The
CMake script accepts the path as a cache variable, so no environment variable is
needed.

## Debugging

OpenOCD with `openocd_pico.cfg` (or `pico.cfg`). Serial console is on the
UART (GPIO 0/1, 115200 baud) and USB CDC. `rp2350.svd` is included for
register inspection in the debugger.

## Interactive console

The firmware provides an interactive command interpreter over the console.
Each command has a full name and a two-letter alias:

| Command | Alias | Action |
|---------|-------|--------|
| `verbose` | `vb` | Toggle verbose logging |
| `status` | `st` | Show verbose / `!RESET` / `!IRQ` state, Core 1 counters, and the register window |
| `examine` | `ex` | Dump the 8-byte register window |
| `modify <off> <byte>...` | `mo` | Write bytes into the window at `<off>` (0..7) |
| `fill [value]` | `fi` | Fill the whole window with `value` (default 00) |
| `usb <args>` | `ub` | USB subcommands |
