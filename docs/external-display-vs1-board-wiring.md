# External display (HDMI) — board-wiring catalog (Track 3, Slice VS1)

Tracker: issue #13 (parent #12). Route: **ADR-0003** — mainline
`drm/bridge/sii902x.c` bridge + a ported **MT6797 DPI** encoder as an
independent second output on the Base tree's mediatek-drm/DDP. Glossary:
**External display**, **SiI9024A**, **right-port mux** (CONTEXT.md).

This is the evidence base for the device tree and driver wiring added in this
slice. Everything here is derived from the vendor trees (not copied verbatim)
and, where possible, cross-checked against the on-device DTB.

## Hardware path

```
MT6797 DDP  ──▶  DPI0 (parallel RGB, 12 data lines, dual-edge/DDR)
             ──▶  SiI9024A HDMI transmitter (I2C3 @ 0x39, TPI register family)
             ──▶  HDMI TMDS ──▶ right-port mux ──▶ right USB-C pins
             ──▶  Planet 30-001-01 pin-remap cable ──▶ HDMI monitor
```

Not USB-C DisplayPort Alt Mode — the mux puts raw HDMI TMDS on the pins, so
generic USB-C→HDMI adapters cannot work (ADR-0003).

## Pin / bus catalog

| Signal            | Line                | Function / notes                                   | Source |
|-------------------|---------------------|----------------------------------------------------|--------|
| I2C control bus   | **I2C3**            | `i2c@11014000`, SCL3=GPIO74, SDA3=GPIO75           | mainline `mt6797.dtsi` `i2c3`, `i2c3_pins_a`; vendor scan |
| Bridge I2C addr   | **0x39**            | 7-bit; vendor `TX_SLAVE_ADDR 0x72` (8-bit) `>> 1`  | vendor `hdmi/sil9024/siHdmiTx_902x_TPI.h:436`; live i2c scan (glossary) |
| Reset             | **GPIO57**          | active-low (vendor `rst_low` holds chip in reset)  | vendor `sil9024a.dtsi` `gpio_sil9022_rst_{high,low}`; on-device `gemini_kali_boot.dts:1222` |
| HPD / EINT        | **GPIO62 / EINT1**  | hot-plug; **not wired as IRQ** (see below)          | vendor `sil9024a.dtsi` `gpio_sil9022_eint_*`; `gemini_kali_boot.dts:1244` |
| 1V2 core enable   | **GPIO247**         | active-high; gates the SiI9024A 1.2 V digital core | vendor `sil9024a.dtsi` `gpio_sil9022_1v2_en_*`; `gemini_kali_boot.dts:1277` |
| DPI data D0..D11  | **GPIO39..GPIO50**  | 12 data lines → `DPI_D0..DPI_D11`                  | vendor `sil9024a.dtsi` `gpio_sil9022_dpi_func1`; `gemini_kali_boot.dts:1400` |
| DPI DE/CK/HS/VS   | **GPIO51..GPIO54**  | `DPI_DE`, `DPI_CK`, `DPI_HSYNC`, `DPI_VSYNC`       | vendor `sil9024a.dtsi`; mainline `mt6797-pinfunc.h` |
| DPI0 block        | **0x1401e000**      | IRQ `GIC_SPI 231` (level-low); DDP DPI0 component  | vendor `mt6797.dts` `dpi0@1401e000`, `dispsys@14000000` |
| OVL1 (ext pipe)   | **0x1400c000**      | IRQ `GIC_SPI 214`                                  | vendor `mt6797.dts` `dispsys` reg/irq list |
| RDMA1 (ext pipe)  | **0x14010000**      | IRQ `GIC_SPI 218`                                  | vendor `mt6797.dts` `dispsys` reg/irq list |

### DPI0 clocks
Vendor `ddp_dpi.c` clocks DPI0 from **TVDPLL** via the `dpi0_sel` mux
(`ddp_clk_set_parent(MUX_DPI0, TVDPLL_D4)`), and enables `DISP1_DPI_MM_CLOCK` +
`DISP1_DPI_INTERFACE_CLOCK`. Mapped onto the mainline `mtk_dpi` `pixel/engine/pll`
triple using the gate parents in `clk-mt6797-mm.c`:

| mtk_dpi clock-name | MT6797 clock                     | rationale |
|--------------------|----------------------------------|-----------|
| `pixel`            | `CLK_MM_DPI_INTERFACE_CLOCK`     | gate parent `dpi0_sel` → TVDPLL (the pixel-rate path) |
| `engine`           | `CLK_MM_DPI_MM_CLOCK`            | gate parent `mm_sel` (register/bus clock) |
| `pll`              | `CLK_APMIXED_TVDPLL`             | the PLL `mtk_dpi` sets the pixel rate on |

*TODO (on-device):* confirm the `pixel`/`engine` assignment and whether an
`assigned-clock-parents` (`dpi0_sel` → a TVDPLL divider) is required so
`clk_set_rate(pll)` actually moves the pixel clock.

## No firmware blob (confirmed)

The SiI9024A is configured purely by **TPI I2C register writes**; EDID is read by
the transmitter over **DDC**. The vendor `drivers/misc/mediatek/hdmi/sil9024/`
tree contains **no `request_firmware`, no `*.bin`** (the only "firmware" hit is a
version *string* `TPI_FW_VERSION`). The mainline `sii902x.c` bridge is likewise
firmware-free. So the mainline bridge is known-sufficient on the firmware axis
(ADR-0003).

## Bus-format check (mainline bridge sufficiency)

`mtk_dpi` `mt8173_output_fmts = { MEDIA_BUS_FMT_RGB888_1X24 }` and
`sii902x_bridge_atomic_get_input_bus_fmts()` returns `{ MEDIA_BUS_FMT_RGB888_1X24 }`
— they **agree**, so the encoder↔bridge atomic format negotiation resolves and
the attach compiles. No `STOP`/reserve-issue-#17 trigger on the negotiated
format.

**Open on-device risk (documented, not a blocker for VS1):** the board wires
only **12 DPI data lines**, and the vendor DPI runs **dual-edge/DDR**
(`ddp_dpi.c`: `DUAL_EDGE_SEL`, `DDR_EN`, `DDR_4PHASE`) to carry RGB888 over
them. Mainline `sii902x.c` hard-programs its TPI input as **24-bit, rising-edge**
(`SII902X_TPI_AVI_PIXEL_REP_BUS_24BIT`, reg 0x09) and never sets its
`SII902X_TPI_AVI_INPUT_BITMODE_12BIT` bit. If on-device bring-up shows the
hardcoded 24-bit input cannot consume the 12-line dual-edge bus, the fix is
either a small `sii902x` input-mode selection (12-bit/dual-edge) driven from the
negotiated format, or — if that proves structural — escalation to the vendor
`sil9024` forward-port (reserve **issue #17**). This is the primary thing the
on-device slice must verify.

## Right-port mux (out of VS1 scope)

HDMI TMDS reaches the USB-C pins only when the **right-port mux** selects HDMI:
GPIO70 (`sw_en`), GPIO71 (`sw_sel` = CC orientation), GPIO72 (`sw7226_en`;
low = HDMI, high = USB). HDMI-out and right-port USB host are mutually exclusive.
Static/DT mux selection and HPD-gated switching are the on-device slice
(PRD #12 user stories 15–17); VS1 does not touch the mux.

## What VS1 wires vs. defers

Wired now (builds + `dtbs_check`):
- `sil,sii9022` bridge node on I2C3 @ 0x39 (reset GPIO57, `cvcc12` = GPIO247-gated
  1V2, `iovcc` = 1V8), OF-graph'd DPI0 → bridge → hdmi-connector.
- `mediatek,mt6797-dpi` DPI0 node + `mtk_dpi` driver support (MT8173 template).
- Independent external DDP pipe (`disp_ovl1`, `disp_rdma1`, `dpi0`) + the
  `mt6797` `ext_path` in `mtk_drm_drv.c`.
- `CONFIG_DRM_SII902X=y` (config fragment).

Deferred to the on-device slice (kept `status = "disabled"` so the working DSI
panel is untouched):
- Enabling the pipe; the **mmsys external-path mux routing** (see porting note in
  `mtk_drm_drv.c`: MT6797 has no direct RDMA1→DPI mux — it needs a UFOE/DSC hop,
  unlike the MT8173 template); the right-port mux; the 12-line/dual-edge input
  resolution above; and all on-hardware validation.
