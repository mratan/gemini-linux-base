# Parked patches — kept for the argument, not applied

`gemini-patch-check.sh` and `gemini-kbuild.sh` glob `*.patch`, so a
`*.patch.parked` file is preserved by git and ignored by the build. Nothing here
is applied.

## `0011-clk-mediatek-hold-the-power-domain-across-probe.patch.parked`
## `0012-clk-resume-runtime-pm-providers-before-the-sweep.patch.parked`

The upstream-shaped fix for #85: give vencsys/vdecsys/imgsys `need_runtime_pm`
so the clk core takes a `pm_runtime` reference on their power domain before
touching a gate, plus upstream's v6.11 reordering so that reference is taken
before `prepare_lock`.

**Measured: both builds that carried them looped on every reboot.** #120 (0011
alone) and #122 (0011 + 0012) each failed 5–7 consecutive boots, where the
unfixed #119 fails about half the time. So the lock inversion 0012 describes was
not the whole story, or not the story at all.

The likely reason is in `scp_domain_data_mt6797[]`: **VDEC, VENC and ISP carry
no `bus_prot_mask`**, while MM does. The vendor's `clk-mt6797-pg.c` does apply
TOPAXI bus protection around these domains. Powering them on and off without
that handshake is a good way to hang the interconnect — which is exactly the
"the MMIO never returns" signature #85 is made of. These patches make the kernel
*toggle* those domains for the first time; before them, genpd only ever powered
them off.

So the direction is right and the mechanism is wrong: the domain must be held,
but this SoC cannot yet be trusted to power these domains back **on**.
`dts/0064` alone does that — attaching the provider with no runtime PM leaves
the device permanently not-runtime-suspended, so `genpd_power_off()` returns
`-EBUSY` and the domain is never powered down, nor ever powered up.

Unpark these only after `mtk-scpsys` learns MT6797's bus protection for
VDEC/VENC/ISP, and prove the domains can be cycled before trusting them.
