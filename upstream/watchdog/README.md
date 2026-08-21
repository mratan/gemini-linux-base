# Staged for upstream — `drivers/watchdog/mtk_wdt.c`

Three defects in one register, split into the three patches a maintainer
would want to review separately. **Not yet sent.**

This is the most obviously upstreamable work in the tree: none of it is
Gemini-specific, none of it needs MT6797 support to exist in mainline first,
and all three are reachable by any MediaTek board whose bootloader arms the
watchdog — which is most of them.

| | Change | Generic? |
|---|---|---|
| 0001 | read the DT mode properties before `mtk_wdt_init()` | yes — pure ordering |
| 0002 | do not inherit the bootloader's `IRQ_EN \| DUAL_EN` | yes |
| 0003 | program `WDT_MODE` in the restart handler | yes |

The board-specific half is already expressible in device tree:
`mediatek,disable-extrst` is an existing upstream property, and 0002 is
what makes it take effect on a watchdog inherited from the bootloader
(today it is honoured only by `mtk_wdt_start()`, which such a watchdog
never reaches).

## Relationship to the in-tree version

`patches/v6.6/misc/0001-watchdog-mtk-wdt-clear-bootloader-irq-dual-mode.patch`
carries the same three changes as one patch with Gemini-specific comments
(B-29/B-30 cross-references). These three are the same code with the
comments rewritten for a reader who has never heard of this device.

Verified equivalent: applying 0001–0003 to a pristine v6.6 `mtk_wdt.c`
produces a file whose code is byte-identical to the in-tree version, with
only comment text differing, and it compiles clean for arm64.

## Before sending

- [ ] `scripts/checkpatch.pl --strict` on all three
- [ ] Rebase onto a current mainline tree (these are against v6.6)
- [ ] Confirm the 490 s and 77 s measurements are reproducible on the
      rebased kernel, so the commit messages stay true
- [ ] `get_maintainer.pl` — expect Matthias Brugger, the watchdog
      maintainers (Wim Van Sebroeck, Guenter Roeck) and linux-mediatek
- [ ] Decide whether 0002's `dev_info()` should be `dev_dbg()`; it is
      genuinely useful once per boot on an affected board, but a maintainer
      may disagree
