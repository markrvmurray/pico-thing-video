# USB Interface Design Notes — pico-thing-video v2.0

Status: design discussion, 2026-08-09; **decision updated 2026-08-10** (dropped
the CC-controller IC for a hand-solderable discrete front end — see below).
Video (HSTX→DVI) and the MC6809 bus peripheral are proven on the v1 dev board;
USB is the last subsystem for the v2.0 board.

## Requirement

- **Primary role — USB *host* for HID:** keyboard **and** mouse, providing the
  human-input side of the retro system's console. ("Screen" is the HSTX→DVI
  video output, already working — USB carries only HID here.)
- **Alternate role — USB *device* (CDC):** an option to present a USB serial
  console *instead* of hosting HID (mutually exclusive with host use).
- Dislike (from v1): the passive **"Y"** that stubs a USB-C and a USB-A
  connector onto the single Pico native USB PHY. Any v2.0 scheme must remove it.

## Core constraint (RP2350 USB)

- The RP2350 has **one native full-speed (12 Mbit/s) USB PHY** on dedicated
  DP/DM pins. It can be **host or device, but only one role at a time.**
- A **second** USB port is possible via **PIO-USB** (bit-banged FS on 2 GPIOs,
  library `Pico-PIO-USB` under TinyUSB), but it requires **clk_sys = 120 or
  240 MHz** — which collides with the video pixel clock (38.4 MHz needs
  clk_sys = 192 MHz, CLKDIV=5) unless HSTX is sourced from a separate PLL
  (the decoupled-HSTX / `FAST_SYS_CLK_320` scaffolding).
- Development flashing is covered by the **SWD debug probe**, and a console by
  **UART0** — so a USB *device* port is a convenience, not a hard dependency.

## Options considered

| Architecture | Connectors | Switching HW | Clocking | Notes |
|---|---|---|---|---|
| Host-only (native PHY) | 1× USB-A | none | 192 MHz | No CDC over USB. Rejected: user wants the CDC option. |
| **Single USB-C, discrete CC** ★ | 1× USB-C | CC resistors + 2×SPDT mux + VBUS switch | 192 MHz | Native PHY, build-time role. All parts SOIC/SOT-23/0603. **Chosen (2026-08-10).** |
| Single USB-C, CC controller | 1× USB-C | PTN5150A (X2QFN) + VBUS switch | 192 MHz | Auto-detect, but 1.6 mm X2QFN too small to hand-assemble. **Rejected on package.** |
| Native-device + PIO-USB-host | USB-C + USB-A | none (2 PHYs) | 240 MHz + decoupled HSTX | Both roles simultaneously; no CC chip. Fallback. |
| C+A + USB 2.0 mux | USB-C + USB-A | mux + VBUS switch | 192 MHz | Mux replaces the Y; still one PHY, one role at a time. |

The **"Y" is the villain**, not the connector count — every option above removes
it (separate PHYs, a mux, or a single connector).

## Chosen: single USB-C, controller-less discrete CC front end (2026-08-10)

Single USB-C, native RP2350 PHY (FS, clk_sys stays **192 MHz**, no PIO-USB),
**role selected at build time** (host image vs CDC-device image, flashed via
SWD). **No CC-controller IC:** every mainstream USB-C CC/DRP controller is
leadless QFN/SON (PTN5150A is a 1.6 mm X2QFN, 0.4 mm pitch — too small to
hand-assemble), so the CC network is built from discrete, hand-solderable parts.

- **CC / role: discrete Rp/Rd switched by a 74LVC2G3157 dual SPDT MUX.**
  One SPDT pole per CC line (CC1, CC2), both thrown together by a single **role
  GPIO** (tie the two selects together if the part breaks them out separately).
  - Common (A) of each pole → the CC pin (CC1 / CC2).
  - One throw → **Rp** (host / DFP): 56 kΩ to 5 V (or 36 kΩ to 3V3) = "default"
    USB current.
  - Other throw → **Rd** (device / UFP): 5.1 kΩ to GND.
  Firmware drives the select to match its build-time role. Switch Ron (a few Ω)
  is negligible against the kΩ CC resistors. (The mux lets one board be either
  role by GPIO and enables a later runtime swap; a hard-wired build could instead
  DNP-populate Rp *or* Rd and omit the mux.)
- **VBUS: current-limited high-side load switch, leaded package** —
  **TPS2051B** in **SOIC-8** (`TPS2051BD`) or SOT-23-5 (`TPS2051BDBV`);
  GPIO-enabled in the host build to source 5 V (hub + HID), off in the device
  build (VBUS sensed only). Fed from the board's system 5 V. **VBUS is the key
  hardware item.**
- **Attach detect (optional):** feed a CC line to the **RP2350 ADC** — no extra
  part — or just always source VBUS in the host build and let TinyUSB enumerate
  on connect.
- **Data: USB 2.0 FS only → tie DP1↔DP2 and DM1↔DM2** straight to the native
  PHY. No mux/redriver; orientation is irrelevant for FS data.
- **ESD:** low-cap TVS array on DP/DM/VBUS/CC.
- **Clocking:** **192 MHz** — native PHY, no PIO-USB.
- **Firmware:** build flag selects `CFG_TUH_ENABLED` (host) or `CFG_TUD_ENABLED`
  (device); at boot it drives the CC-select GPIO and the VBUS-enable GPIO to
  match its role. No CC/PD driver, no I²C, no role detection.

Everything in the USB-C front end is now SOIC / SOT-23 / 0603 — nothing finer
than the 74LVC2G3157 (dual SPDT) and the TPS2051B (SOIC-8), both hand-solderable.
Trade-off vs a CC controller: no *automatic* host/device detection — role is
fixed per firmware build, which is acceptable (build-time role was agreed).

### HID device count (independent of connector choice)

Keyboard + mouse = **two** USB devices. A single host port serves both via
either:
- a downstream **USB hub** (TinyUSB host supports hubs), or
- a **wireless combo receiver** (one dongle enumerating as both HID) — no hub,
  mechanically cleanest.

Plan for a hub; validate first with a combo dongle.

## Fallback: two fixed-role ports (no switching, no Y)

If a CC controller is undesirable (supply, firmware, cost):
- **USB-A = host** (kbd/mouse via hub) on **PIO-USB** (2 GPIOs, PIO2 — pio0/pio1
  are the bus SMs).
- **USB-C = device** (CDC) on the **native** PHY (fixed Rd, VBUS sense only).

No CC chip, trivial VBUS (A always sources, C always sinks), and **both roles
live at once** (HID *and* console simultaneously, not "instead"). Costs: two
connectors, PIO2, and the **240 MHz + decoupled-HSTX** clock tree (PIO-USB
needs 120/240 MHz). Only wins if simultaneous host+console is actually wanted.

## TinyUSB role handling (bundled stack: 0.18)

- **One native port = one role at a time.** The RP2350 PHY is host *or* device,
  chosen at init (`HOST_NDEVICE` bit); TinyUSB's `dcd_rp2040.c` and
  `hcd_rp2040.c` both drive the same hardware. It cannot be both at once.
- **Build-time role (the chosen approach):** compile with `CFG_TUH_ENABLED`
  **or** `CFG_TUD_ENABLED` and bring up `tuh_*` or `tud_*` accordingly. Simplest
  and most robust — no detection, no teardown, no re-enumeration.
- **Runtime switch on one port: possible, but not seamless.** TinyUSB 0.18
  provides `tud_deinit(rhport)` / `tuh_deinit(rhport)`, so a live swap is
  `tud_deinit(0)` → flip VBUS → `tuh_init(0)` (or the reverse). It is a full
  re-init cycle (the bus re-enumerates), triggered by an event, and `deinit` is
  newer / less battle-tested. If ever needed, the robust pattern is
  **reboot-into-role** (detect role, watchdog-reset into the matching stack)
  rather than a live `deinit`/`init`. Build-time role avoids all of this.
- **Both roles at the same time: only via two rhports** — native device on
  `rhport 0` + PIO-USB host on `rhport 1`. TinyUSB fully supports concurrent
  device + host this way (the two-fixed-port fallback), at the cost of the
  120/240 MHz PIO-USB clock.

## CC-controller option (shelved — all leadless QFN/SON, too small to hand-solder)

A CC controller would add *automatic* host/device detection, but every mainstream
option is a leadless QFN/SON too fine to hand-assemble — so this whole class is
**shelved** unless a future rev goes to assembled fab (JLCPCB / hot-air + stencil).
Kept for reference. If revisited, **PTN5150A** (pure CC logic, no PD → light
firmware) is the pick; FUSB302B works but is a full PD PHY needing a ported
driver (overkill — no PD needed). Datasheet + KiCad symbol/footprint for the
PTN5150A already live in `datasheets/` and `kicad/`.

| Part | Vendor | I/F | Package | Notes |
|---|---|---|---|---|
| **PTN5150A** | NXP | I²C | X2QFN12, 1.6 mm | Best fit *electrically*; rejected on size. CC logic + DRP + orientation, no PD. |
| FUSB302B | onsemi | I²C | WQFN-14 | Full Type-C **PD** PHY; DRP; needs a ported CC/PD stack. Overkill (no PD). |
| HUSB320 | Hynetek | I²C / strap | QFN | TUSB320-class DRP; cheap, strong LCSC stock (JLCPCB assembly). |
| WUSB3801 | Will Semi | I²C / strap | QFN/USON | Type-C DRP port controller; LCSC-stocked. |
| TUSB320LAI / TUSB320I | TI | I²C / strap | WQFN/X2SON | Same family; all leadless. |

All leadless — none in SOIC/TSSOP; that class simply doesn't make a leaded CC
controller. Not suitable at all (sink-only PD triggers, cannot present a host/DFP
role): CH224K, AP33771/AP33772, HUSB238.

The **controller-less discrete front end above is the chosen path** and needs
none of these. The two-fixed-port fallback (native device + PIO-USB host) also
needs no CC controller, if simultaneous host + console is ever wanted.

## Open decisions

Settled (2026-08-10): single USB-C, native PHY (192 MHz), **controller-less
discrete CC** (Rp/Rd via a **74LVC2G3157** dual SPDT) + **TPS2051B** VBUS load
switch, DP/DM tied, **build-time role**. Remaining:

1. Rp value / current advertised: 56 kΩ→5 V (or 36 kΩ→3V3) for "default" USB —
   fine for HID; confirm nothing wants 1.5 A.
2. Attach detect: wire a CC line to the **RP2350 ADC**, or just always-source
   VBUS in the host build and let enumeration handle it?
3. HID via a downstream **USB hub** or a **wireless combo dongle** for first
   bring-up (dongle = one device, no hub)?
4. Pick GPIOs: one for the **74LVC2G3157 select** (CC role) and one for the
   **VBUS-switch enable**; confirm the 74LVC2G3157 select topology (shared vs
   two selects tied together) from its datasheet.
5. Keep the **two-fixed-port** design (native device + PIO-USB host, concurrent
   HID *and* console at 240 MHz) on the shelf, or commit to single-C only?

## Key part references

Chosen (discrete front end, all hand-solderable):
- **CC role mux: 74LVC2G3157** dual SPDT (one pole per CC line, role GPIO select)
- **CC resistors:** Rd 5.1 kΩ (device), Rp 56 kΩ→5 V / 36 kΩ→3V3 (host), on CC1 + CC2
- **VBUS load switch: TPS2051B** — SOIC-8 (`TPS2051BD`) or SOT-23-5 (`TPS2051BDBV`); alts TPS2065, MIC2005
- **ESD:** low-capacitance USB TVS array (DP/DM/CC/VBUS)

Shelved / reference:
- CC controllers (all leadless QFN/SON): PTN5150A, TUSB320(I/LAI), FUSB302B, HUSB320, WUSB3801
- 2nd-port option: `Pico-PIO-USB` (sekigon-gonnoc) + TinyUSB host, clk_sys 120/240 MHz
