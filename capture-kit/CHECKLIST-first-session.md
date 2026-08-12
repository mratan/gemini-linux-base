# First-session checklist — Reference-slot captures (C0a → C0b → C1)

**GATE: every step below is physical-session work. Do NOTHING here until
the user has declared the remote-only period ended and the device in
hand.** Until then all artifacts stay staged and NOT-YET-FLASHED.

Standing safety rules (permanent, non-negotiable):
- never `mtk wl`, never anything touching the GPT;
- never flash or modify `nvram`/nvdata;
- writes are single-partition targeted only;
- **boot2 (Reference slot) is never flashed — ADR-0002.** The
  instrumented kernel, although boot2-compatible by construction, goes
  to **boot3 only**.

Slot keys (README.md "Boot slots"):
| Slot | Key at power-on | Contents |
|---|---|---|
| boot1 | none | rooted Android |
| boot2 | hold **silver side button** | Gemian (Reference slot — untouchable) |
| boot3 | hold **silver + Esc** | UBports → replaced by the capture kernel this session |
| recovery | hold Esc | stock recovery |

Bring to the session: `capture-kit/staging/boot3-capture-gemian.img`
(+ `SHA256SUMS`), `capture-kit/device-scripts/*`, a copy of
`04-docs/DIVERGENCE-DEBUG-PLAN.md`, mtkclient workstation, USB cable,
(optional) FTDI serial rig @ 921600.

---

## 0. Preconditions

- [ ] Remote-only period declared over by the user.
- [ ] Verify staged image integrity: `sha256sum -c SHA256SUMS`.
- [ ] Boot **boot2** (silver) normally; SSH in. Confirm Reference slot
      healthy: `uname -a` → `3.18.41+`, Wi-Fi works
      (`connmanctl technologies`, scan). This is the last normal boot
      before hold-off; abort the whole session if the Reference slot is
      not healthy.
- [ ] Copy `device-scripts/` to the device (e.g.
      `scp -r device-scripts gemini@<ip>:~/capture-kit/`), `chmod +x`.
- [ ] Confirm boot3's UBports TWRP backup still exists
      (`/home/gemini/boot3-twrp-backup.img` on Gemian, per README) —
      it is the boot3 restore path.

## 1. Daemon hold-off (Reference slot userspace, reversible)

Mechanism (evidence: Gemian rootfs inspection 2026-08-12, see
`capture-kit/README.md` §Daemon hold-off): Gemian is Debian 9/systemd;
the vendor WMT daemons run **inside the Android LXC container** started
by `lxc@android.service` + `droid-hal-init.service`
(multi-user.target.wants). The kernel side is inert without them
(`MTK_WCN_REMOVE_KERNEL_MODULE`: all connectivity driver init waits for
wmt_loader's ioctl).

- [ ] `sudo systemctl mask lxc@android.service droid-hal-init.service`
- [ ] `sudo reboot` — still into **boot2** (silver). SSH in.
- [ ] Verify held: `systemctl is-active lxc@android.service` →
      inactive; `ls /dev/stpwmt` → absent;
      `pgrep -f 'wmt_loader|wmt_launcher'` → nothing.
- [ ] **Bonus baseline (stock kernel):**
      `sudo sh ~/capture-kit/capture-c0a.sh --skip-modinit`
      (devmem rows only; PMIC/EMI-CRC rows will read SKIPPED — expected).
- [ ] `sudo poweroff`.

## 2. Flash the capture kernel to boot3 (the only flash of this phase)

- [ ] Device to preloader/BROM; mtkclient session (physical presence
      rule satisfied by definition of this session).
- [ ] Optional but recommended: read back current boot3 first,
      `mtk r boot3 boot3-ubports-asflashed-<date>.img` (belt+braces on
      top of the TWRP backup).
- [ ] `mtk w boot3 boot3-capture-gemian.img` — **boot3, never boot2**.
      Single-partition targeted write. Verify by read-back + sha256
      compare against `SHA256SUMS`.
- [ ] ABORT PATH: if the flash fails/hangs — power off, recover per
      standard mtkclient stuck-DA procedure (Esc + silver), retry.
      boot2 is untouched at all times.

## 3. Boot the capture kernel (boot3 = silver + Esc)

- [ ] Power on holding **silver + Esc**. (Optional: serial rig attached;
      the capture kernel never mutes UART, patch 0001/bsg100-0003.)
- [ ] SSH in (same Gemian userspace, daemons still masked — the mask
      lives in the shared rootfs).
- [ ] Verify: `uname -a` → `3.18.41 … #1 SMP PREEMPT … 2026` (our
      banner, NOT `3.18.41+ … 2019`); `[ -w /proc/driver/wmt_capture ]`.
- [ ] Verify held again: no /dev/stpwmt, no wmt processes.
- [ ] ABORT PATH (kernel doesn't boot / no SSH): power off; boot
      **boot2** (silver) — the Reference slot still works; collect
      serial log if rigged; session falls back to stock-kernel captures
      (C0a devmem + C1 without kernel instrumentation) while the kernel
      issue is debugged off-line. Restore boot3 from backup at leisure.

## 4. C0a — Pre-firmware capture

- [ ] `sudo sh ~/capture-kit/capture-c0a.sh`
      (sequence points c0a-pre-modinit and c0a-post-modinit; modinit =
      wmt_loader-equivalent driver registration, powers nothing).
- [ ] Sanity: `c0a-regtable.txt` has CONN rows `SKIPPED-CONN-UNPOWERED`
      (LK leaves CONSYS cold) and `/proc/driver/wmt_dbg` exists after.

## 5. C0b — the single-query experiment

- [ ] `sudo sh ~/capture-kit/capture-c0b.sh`
- [ ] Read the verdict line:
      `CAPTURE-INIT: step="query stp default" result=PASS` → **ROM
      answers pre-push** (decision tree: diff C2 vs C0a next session);
      `result=RX-FAIL` → ROM does not answer pre-push (decision tree:
      bisect the C1 transition).
- [ ] ABORT PATH: if the func-on attempt wedges the WMT core (no
      verdict line, wmtd stuck — not expected: the patch search
      times out in 2000 ms): `sudo reboot` back into boot3 (daemons
      still masked), skip to step 6; C0b evidence up to the wedge is
      already in the saved dmesg.

## 6. C1 — working-transition capture (same boot cycle if possible)

- [ ] `sudo sh ~/capture-kit/capture-c1.sh`
      (unmasks + starts `lxc@android.service` then
      `droid-hal-init.service`; Android init runs wmt_loader →
      wmt_launcher in vendor order; script ensures Wi-Fi function-on and
      reports trace volume vs the H35 reference counts).
- [ ] Verify the trace contains: `CAPTURE-MCU-RST: release`,
      `HARVEST-WMT-TX`, `HARVEST-BTIF-RX` lines, and
      `CAPTURE-INIT: … result=PASS` for the init tables.
- [ ] Associate to an AP: `connmanctl` (`enable wifi; scan wifi;
      services; agent on; connect <service>`), confirm an IP.
- [ ] `sudo sh ~/capture-kit/capture-post-assoc.sh`

## 7. Retrieval, scrubbing, restore

- [ ] Pull captures: `scp -r gemini@<ip>:/home/gemini/captures/<date> ./raw-captures/`
      (keep this raw copy OUTSIDE git).
- [ ] Scrub a copy for the repo:
      `sh device-scripts/scrub-capture.sh copy/*`; verify no MACs/IMEIs
      remain; place under `04-docs/captures/<date>/` in gemini_linux and
      commit (text only).
- [ ] Decide daemon mask end-state: for a device that should keep
      working Wi-Fi on the Reference slot, leave unmasked (C1 already
      unmasked them). If more held-off boots are planned, re-mask and
      note it in the session log.
- [ ] boot3 disposition: leave the capture kernel (it boots Gemian; C2
      work will replace it with the 6.6 experimental image later) or
      restore UBports from the TWRP backup — record the choice.
- [ ] Update `04-docs/STATE-<date>.md` with the new device state.

## Session outputs checklist

- [ ] c0a-regtable.txt + c0a-dmesg.txt (2 sequence points)
- [ ] c0b-regtable.txt + c0b-dmesg-experiment-window.txt + verdict
- [ ] c1-regtable.txt + c1-dmesg-transition-window.txt (the H35-diffable
      golden trace **with pre-firmware baseline attached** — the thing
      bsg100 never had)
- [ ] post-assoc snapshot
- [ ] all scrubbed copies committed to `04-docs/captures/<date>/`
