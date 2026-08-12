# Right-port USB-C mux — HDMI / USB-host selection (Track 3, Slice VS2)

Tracker: issue #16 (parent #12). Builds on **VS1** (#13), which added the
`sil,sii9022` bridge, the MT6797 DPI encoder and the DPI/HDMI pinctrl to the
board DTS. Route: **ADR-0003**. Glossary: **Right-port mux** (CONTEXT.md).

## What the mux is

The Gemini's **right** USB-C port is a hardware mux: its pins carry **EITHER**
HDMI TMDS (from the on-board SiI9024A transmitter) **OR** USB-2 host — **never
both**. This is not USB-C DisplayPort Alt Mode; only the Planet 30-001-01
pin-remap cable drives the HDMI path (ADR-0003).

```
                         ┌──────────────► HDMI TMDS ► SiI9024A ► right USB-C pins
right-port mux ──(sel)───┤   (sw7226_en = low)
                         └──────────────► USB-2 host ► right USB-C pins
                             (sw7226_en = high)
```

## Control GPIOs (evidence)

Authority: vendor `ubports-3.18`
`drivers/misc/mediatek/usb_c/fusb302/usb_typec.c`
(`fusb300_gpio_init` / `fusb300_eint_work`; `aeon_gpio` names in parens).

| Line       | GPIO   | aeon_gpio name     | Role                                   |
|------------|--------|--------------------|----------------------------------------|
| `sw-en`    | GPIO70 | `fusb301a_sw_en`   | OTG/HDMI lane-mux enable               |
| `sw-sel`   | GPIO71 | `fusb301a_sw_sel`  | CC orientation (CC1 = low, CC2 = high) |
| `sw7226-en`| GPIO72 | `sw7226_en`        | **master select: low = HDMI, high = USB** |

Vendor output values (logical / active-high):

| Mode       | sw_en (70) | sw_sel (71)     | sw7226_en (72) |
|------------|:----------:|:---------------:|:--------------:|
| `usb-host` |     1      |       0         |       1        |
| `hdmi`     |     0      | CC orient (0=CC1)|      0        |

`usb1_drvvbus` (GPIO94) is VBUS, driven high in both vendor modes; it is **not**
part of the lane mux and stays a static gpio-hog (from the right-port-host slice
`patches/v6.6/dts/0014`).

## Mutual exclusion — how it is enforced

`sw7226_en` (GPIO72) is the **master interlock**: no value of it routes both
HDMI and USB, so the two are mutually exclusive **by construction**. The
`gemini-rightport-mux` driver owns all three lines and applies exactly one mode
at a time under a mutex — a single `mode` write drives the whole trio to one
consistent state. There is no code path and no GPIO combination that enables
both. Practical consequence: a right-port **USB dongle** (e.g. the Wi-Fi /
ethernet fallback) cannot be used while **HDMI** is selected, and vice-versa.
Acceptable because Internal Wi-Fi is the Prototype gate (ADR-0003).

## How the mode is selected

Two mechanisms, both **decoupled from the Type-C/fusb302 state machine**
(ADR-0003) — the mode is a DT default plus a runtime sysfs write, **not** driven
by CC attach or HPD:

1. **DT default** — `planet,default-mode` on the `rightport-mux` node
   (`patches/v6.6/dts/0024`). Ships as `"usb-host"` so the right port keeps its
   pre-VS2 host behaviour at boot; set to `"hdmi"` to boot HDMI-routed.

2. **Runtime sysfs** — the driver exposes a `mode` attribute:

   ```sh
   cat  /sys/devices/platform/rightport-mux/mode   # -> hdmi | usb-host
   echo hdmi     > /sys/devices/platform/rightport-mux/mode   # route HDMI
   echo usb-host > /sys/devices/platform/rightport-mux/mode   # route USB host
   ```

   (The exact sysfs path is the platform device's directory; find it with
   `grep -l . /sys/devices/platform/*/mode` or via the driver's
   `/sys/bus/platform/drivers/gemini-rightport-mux/` link.)

## Pinctrl states

The DTS defines a single pin group (`rightport_mux_pins`) muxing GPIO70/71/72 to
plain **GPIO function**, referenced by both the `hdmi` and `usb-host` pinctrl
states. Upstream `pinctrl-mt6797` implements **pinmux only** (no pinconf /
output-value), so the two states are pinmux-identical and the modes differ
solely in the **output values** the driver drives — the same convention the VS1
`dpi`/`hdmi` pin groups follow. The driver still selects the matching state per
mode so that intent is expressed in the DT and a future pinctrl-mt6797 with
pinconf can carry per-mode bias without a driver change.

## Out of scope (later polish)

**Automatic HPD-gated switching** — replicating the vendor
`fusb300_eint_work` (CC attach → 5 ms settle → read `sil9022` HPD → mux, with
CC1/CC2 orientation on `sw_sel`) — is **tracker #19** (ready-for-human), not
implemented here. VS2 provides only the static/sysfs selection and the interlock.

## What VS2 adds

- `patches/v6.6/misc/0001-misc-add-gemini-rightport-mux-select-driver.patch` —
  the `gemini-rightport-mux` platform driver (`drivers/misc/`), its Kconfig
  (`GEMINI_RIGHTPORT_MUX`) / Makefile entries, and the DT binding
  `Documentation/devicetree/bindings/misc/planet,gemini-rightport-mux.yaml`.
- `patches/v6.6/dts/0024-arm64-dts-mediatek-gemini-right-port-mux-select.patch`
  — disables the three mux gpio-hogs from `0014` (freeing GPIO70/71/72 for the
  driver), adds the `rightport-mux` node and the `rightport_mux_pins` pinctrl
  group, and documents the mutual exclusion in-tree.
- `configs/gemini-rightport-mux.config` — `CONFIG_GEMINI_RIGHTPORT_MUX=y`.
- CI (`kernel-build.yml`) `dtbs_check` gate extended to validate the mux node
  against its new binding.
