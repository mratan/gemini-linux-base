# Coverage — divergence-debug plan vs capture kit

Line-by-line review of `04-docs/DIVERGENCE-DEBUG-PLAN.md` against this
kit. Two implementations exist for most rows: the **kernel** side
(`patches/0002-…​.patch`, function `wmt_capture_regtable()` /
`wmt_capture_cpupcr_burst()` in `mtk_wcn_consys_hw.c`) emits at the
bring-up-internal sequence points; the **script** side
(`device-scripts/capture-lib.sh`, function `regtable()` /
`cpupcr_burst()`) reads the same rows via devmem at the operator-driven
sequence points. Line references: `P` = `patches/0002-capture-full-…​.patch`,
`L` = `device-scripts/capture-lib.sh`.

## Register table ("What a Pre-firmware capture must record")

| Plan row (group / register) | Kernel side | Script side | Notes |
|---|---|---|---|
| MCU exec: CONSYS_CPUPCR 0x18070160, ≥32 samples @~1 ms | P:224 (table row), P:428 burst fn, P:484 burst at mcu-reset-release | L:114 (single row), L:201 `cpupcr_burst()` (kernel-hook preferred, devmem fallback) | Burst fires in-kernel at the decisive point (reset-release), 32×`usleep_range(950,1050)`; also operator-triggerable (`cpupcr <tag>`) |
| Identity: chip ID 0x18070008 (0x0279) | P:222 | L:112 | gated on CONN power (both sides) |
| Identity: HW_VER (0x18070000, expect 0x8A00) + FW_VER 0x18070004 | P:220–221 | L:110–111 | |
| Identity: MCU_CFG_ACR 0x18070110 | P:223 | L:113 | |
| Power: SPM_CONN_PWR_CON **0x1000632C** (golden 0x10D) | P:199 | L:91 | stale 0x10006280 additionally captured (P:200, L:92) per the plan's stale-define-trap note — capture, never poke |
| Power: SPM_PWR_STATUS 0x10006180 / 2ND 0x10006184 (bit 1) | P:197–198 | L:89–90; also the gate in `conn_powered()` L:63 | |
| Power: SPM_PWRON_CONFG_EN 0x10006000 | P:196 | L:88 | |
| Bus protect: INFRA_TOPAXI_PROT_EN 0x10001220 / PROTECTSTA1 0x10001228 (bits 17\|18) | P:201–202 | L:93–94 | |
| AP↔CONN misc: 0x10001f00 (bit 11 lead) | P:203 | L:95 | also every HARVEST-SNAP line (patch 0001, H35 format) |
| AP↔CONN misc: TOPCKGEN 0x10001350 sleep mask (bit 8) | P:205 | L:97 | |
| AP↔CONN misc: WDT SWSYSRST 0x10007018 (bit 12) | P:206; assert/release **events** P:462/P:482 | L:98 | CAPTURE-MCU-RST event lines = the plan's "MCU reset assert/release" event row |
| EMI: CONSYS_EMI_MAPPING 0x10001340 | P:204 | L:96 | |
| EMI: reserved-memory phys base | `CAPTURE-EMI: … gConEmiPhyBase` (P:~340) | — (kernel only; not a devmem-reachable symbol) | |
| EMI: hash/first-64-bytes of 343 KB ctrl window | P:333–357: CRC32 over 343 KiB + 64-byte hexdump | L:178–190: first 64 bytes via devmem | kernel gives the hash; script gives an independent word-level read |
| BTIF: full block 0x1100C000 — IER, IIR, FIFOCTRL, LSR, FAKELCR, SLEEP_EN, DMA_EN, RTOCNT, TRI_LVL (golden 0x18), WAK +0x64, WAT_TIME, HANDSHAKE (golden 0x3) | P:233–243 | L:127–146 | FIFOCTRL is write-only (shares 0x8 with IIR on read) — recorded as `BTIF_IIR_FIFOCTRL`, documented in both sources |
| BTIF: DMA enables | BTIF_DMA_EN P:238 + APDMA TX/RX EN + INT_EN P:246–249 | L:132, L:152–160 | APDMA rows gated on CLK_INFRA_AP_DMA (INFRA_CG_STA1 bit 18) |
| BTIF: CLK_INFRA_BTIF gate state | INFRA_CG_STA0 0x10001090 row (P:207-area) + gating logic | L:70 `btif_ungated()`, row L:99 | gate register itself captured, per "capture, don't re-poke" |
| Clock/XO: PMIC DCXO CW00 (bit 5 XO_WCN) | P:257 via `pwrap_read` | L:165 → delegated to kernel hook (devmem cannot reach pwrap targets) | the row the plan's kernel-side hook requirement exists for |
| Clock/XO: pwrap DCXO_CONN bridge regs | P:209–213 | L:100–104 | |
| Clock/XO: VCN LDO states (MT6351 0x0A52/0x0A0C/0x0A92) + RG_VCN28_ON_CTRL | P:258–260 (`pwrap_read`; ON_CTRL bit lives in LDO_VCN28_CON0, comment P:259) | L:165–174 → kernel hook | addresses verified against `upmu_hw.h` in the gemian tree |
| Timing: uptime timestamps of power-on, MCU reset release, first BTIF TX, first RX byte | printk timestamps on: reg_ctrl entry (HARVEST-SNAP + CAPTURE-SNAP-BEGIN), CAPTURE-MCU-RST release (P:482), HARVEST-BTIF-TX / HARVEST-BTIF-RX first lines (patch 0001) | dmesg harvested by every capture script | golden latency reference: ROM answers ~50 ms after release |

## Sequence points ("sampled at defined sequence points, not just once")

| Sequence point | Where emitted |
|---|---|
| c0a-pre-modinit, c0a-post-modinit | `capture-c0a.sh` (script regtable + kernel `snap` at each) |
| reg_ctrl-on-entry / reg_ctrl-off-entry | kernel, patch 0002 hook (+ HARVEST-SNAP from patch 0001) |
| pre-chipid-poll | kernel hook |
| **mcu-reset-release** (+ CPUPCR burst) | kernel hook — the exact uncaptured window from bsg100's B-21 postmortem |
| after-reg-ctrl-on | kernel hook |
| c0b-pre-funcon, c0b-post-attempt | `capture-c0b.sh` |
| c1-pre-release, c1-post-bringup | `capture-c1.sh` |
| c1-post-associate | `capture-post-assoc.sh` |

## Event instrumentation (C1/C2, H18/H35-compatible)

| Plan event row | Implementation |
|---|---|
| every BTIF TX/RX byte, timestamp + direction | patch 0001 (bsg100 0002 verbatim): `HARVEST-BTIF-TX/RX` hexdumps — byte-format identical to H18/H35 |
| WMT command/event boundaries with opcode | patch 0001: `HARVEST-WMT-TX/RX` hexdumps (opcode = byte 1 of the dump) |
| STP mode transitions (mand ↔ full) | patch 0002: `CAPTURE-STP-MODE: old=… new=…` in `mtk_wcn_stp_set_mode()` |
| FIFO clear/reset operations | patch 0002: `CAPTURE-BTIF-FIFO-CLR: rx\|tx (hw_init\|fifo_reset)` in `btif_plat.c` |
| MCU reset assert/release | patch 0002: `CAPTURE-MCU-RST: assert\|release` at both `mtk_wdt_swsysret_config` sites |
| each `init_table_*` step + pass/fail | patch 0002: `CAPTURE-INIT: step="…" start / result=PASS\|TX-FAIL\|RX-FAIL\|EVT-MISMATCH` + `script-done` summary in `wmt_core_init_script()` |

H18/H35 diffability: all `HARVEST-*` lines are format-identical to the
mirrored traces (patch 0001 is bsg100's own code); new information uses
the distinct `CAPTURE-*` prefix so a plain
`normalize-capture.sh trace <log> | grep -v '^CAPTURE-'` yields a stream
directly diffable against the timestamp-stripped H18/H35 logs.

## Diff-method support

- Plan step 1 (normalize to `register, sequence-point, value`):
  `device-scripts/normalize-capture.sh table` emits exactly that CSV
  from both script output and dmesg; `trace` mode does the
  timestamp-strip for trace diffs.
- Standing rules: capture files land in `04-docs/captures/<date>/`
  (scripts create dated dirs), commit as text **after**
  `scrub-capture.sh` (MAC/IMEI/ESSID scrubbing).

## Known deltas / judgment calls (reviewed, deliberate)

1. **BTIF_WAK read**: documented write-only; read attempt kept because
   bsg100's golden harvest read it too (comparability beats purity).
2. **The CPUPCR burst at mcu-reset-release adds ~35 ms** between reset
   release and the first BTIF TX. Timing rows still localize (all
   timestamps recorded); the golden ~50 ms ROM-answer latency is
   measured from the release line, unaffected by when the query is
   finally sent. Accepted as the cost of sampling the decisive window;
   bsg100's observer-effect lesson (H28) capped the volume (32 lines,
   process context, no IRQ-path logging).
3. **C0a on the stock (uninstrumented boot2) kernel** covers only the
   devmem-reachable rows (PMIC/EMI-CRC rows report
   `SKIPPED-NEEDS-KERNEL-HOOK`). Full C0a coverage uses the capture
   kernel booted from boot3 with the same Gemian userspace. Both modes
   are supported so the session can take a bonus stock-kernel baseline
   first.
4. **C0b uses func-on(WIFI) via the stock `wmt_dbg` 0x7 interface**, not
   a bespoke single-query injector: the first thing sw_init does on
   BTIF is the pre-patch `init_table_1_2` query (wmt_ic_soc.c:1011),
   and the launcher-less patch search self-terminates in 2000 ms
   (wmt_ctrl.c:423) — vendor code end-to-end, single variable.
