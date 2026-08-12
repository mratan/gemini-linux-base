# First-session RUNBOOK — flash + bring up the Experimental slot

Slice 10 (tracker issue #11). This is the ordered procedure for the **first
physical session**: back up boot3, capture the Reference slot, flash the
Experimental slot, and attempt first CONSYS bring-up. It assembles into one
place what the earlier slices produced; the deliverable it flashes is built by
`release/assemble.sh` and pinned by `release/MANIFEST.txt`.

---

## ⛔ GATE — do not execute any step below yet

**Nothing in this runbook runs until the user has declared the remote-only
period over and the device is in hand.** Until then every artifact stays
staged and **NOT-YET-FLASHED**. Assembling images, checking hashes, and
reading this document are device-free and fine; anything that writes the
device is not.

If you are reading this during the remote-only period: stop at the end of
"Part 0 (device-free prep)". Do not proceed to Part 1.

---

## Standing safety rules (permanent, non-negotiable — apply to every step)

- **Never `mtk wl`; never any operation that writes the GPT.** Only
  single-partition targeted writes (`mtk w boot3 …`) and plain file copies.
- **Never flash or modify `nvram` / `nvdata`** (IMEI, Wi-Fi calibration/MAC).
  The running system reads them in place; nothing here touches them.
- **Never flash boot2, and never write the `linux` partition's own Gemian
  rootfs** (ADR-0002). boot2 + Gemian = the Reference slot; it is the
  instrument the whole Wi-Fi strategy depends on and must survive intact.
- The Experimental system is **boot3 kernel + a loop-image FILE
  (`/experimental.img`) placed BESIDE Gemian on the `linux` partition** —
  exactly like the existing `ubuntu.img` precedent. Copying that file is a
  filesystem write, never a partition-level operation.
- **Single-variable changes, one build per change** during bring-up (kept from
  bsg100's discipline). Every capture is committed as text only, **after**
  scrubbing device-unique identifiers (MACs, IMEIs).

## Boot slots and recovery combo (README.md "Boot slots")

| Slot | Key held at power-on | Contents |
|---|---|---|
| boot1 | none | rooted Android |
| boot2 | **silver** side button | Gemian 3.18.41 — **Reference slot, untouchable** |
| boot3 | **silver + Esc** | UBports (parked) → Experimental kernel this session |
| recovery | **Esc** | stock recovery |

**Stuck-DA / mtkclient recovery:** power off, hold **Esc + silver** to re-enter
the download path, retry. boot2 is untouched at all times, so the device is
never bricked by anything in this runbook — worst case you restore boot3 from
its backup (Part 1) and boot Gemian (silver).

---

## Part 0 — device-free prep (safe now)

1. **Assemble the deliverable** (no root needed for the boot3 image + manifest;
   the rootfs loop image needs root / CI — pull it from the `release` CI
   workflow artifact, or build locally under a root context):

   ```sh
   # real firmware (private blobs), local kernel tree + built modules:
   release/assemble.sh \
     --kbuild /path/to/linux-6.6 \
     --kernel-dir /path/to/kernel-build-artifact \
     --payload /path/to/05-gemini-payload/consys \
     --busybox /path/to/static-arm64-busybox \
     --build-rootfs        # omit if no root; take experimental.img from CI
   ```

   Output lands in the gitignored `release/out/`:
   `boot3-experimental.img`, `experimental.img`, `MANIFEST.txt`,
   `NOT-YET-FLASHED.txt`.

2. **Verify the deliverable against the manifest.** Confirm every hash in
   `release/out/MANIFEST.txt` matches what you carry to the session, and that
   the four firmware blobs match the catalog
   (`04-docs/PAYLOAD-CATALOG.md` / `scripts/verify-payload.sh`).

3. **Bring to the session:**
   - `release/out/boot3-experimental.img` + `experimental.img` (+ `MANIFEST.txt`);
   - the capture kit: `capture-kit/staging/boot3-capture-gemian.img`
     (+ its `SHA256SUMS`), `capture-kit/device-scripts/*`;
   - a copy of `04-docs/DIVERGENCE-DEBUG-PLAN.md`;
   - mtkclient workstation, USB cable, (optional) FTDI serial rig @ 921600.

> The rest of this runbook is **physical-session work — gated on the remote-only
> period being over.**

---

## Part 1 — back up the current boot3  *(first thing on the device)*

**Why first:** everything else in this session either writes boot3 or depends
on being able to fall back to it. Back it up before touching anything.

- [ ] Boot **boot2** (silver), SSH into Gemian, confirm the **UBports TWRP
      backup still exists** (`/home/gemini/boot3-twrp-backup.img`, per README) —
      that is restore path #1.
- [ ] Belt-and-braces read-back of the live boot3 via mtkclient (download mode,
      **read only**):
      ```sh
      mtk r boot3 boot3-ubports-asflashed-$(date +%Y%m%d).img   # READ, never write
      ```
      Keep this file **outside git** (it is a device image). This is restore
      path #2.
- **Risk:** none beyond entering download mode (read-only). **Abort/recovery:**
  if mtkclient wedges, power off, **Esc + silver**, retry; the device still
  boots boot2 (silver) unchanged.

## Part 2 — capture the Reference slot (C0a → C0b → C1)

**Why before flashing:** the Pre-firmware capture (C0) is only reachable on the
Reference slot with the daemons held off, and it is the single most
load-bearing piece of evidence for the whole bring-up (see
`04-docs/DIVERGENCE-DEBUG-PLAN.md`, "Why this plan exists"). Take it while the
Reference slot is pristine, before the Experimental slot exists.

- [ ] **Follow `capture-kit/CHECKLIST-first-session.md` in full.** Do not
      duplicate it here — it is the authority. In summary, that checklist:
  1. confirms the Reference slot is healthy (boot2, Wi-Fi works) — **abort the
     whole session if it is not**;
  2. masks the vendor WMT daemons (`lxc@android.service`,
     `droid-hal-init.service`) so CONSYS stays cold — reversible;
  3. flashes the **instrumented capture kernel to boot3 only** (never boot2),
     with a read-back verify — *this is the only flash of Part 2*;
  4. runs **C0a** (pre-firmware register table), **C0b** (the single
     `WMT_QUERY_STP` experiment — the verdict line says whether the ROM answers
     pre-push), and **C1** (release the daemons, capture the working transition
     + Wi-Fi function-on);
  5. pulls captures, **scrubs MACs/IMEIs**, commits text-only to
     `04-docs/captures/<date>/`.
- **Risk:** the capture kernel is flashed to boot3 (Part 1 already backed boot3
  up). The Reference slot is never flashed. **Abort/recovery:** if the capture
  kernel does not boot, power off, boot **boot2** (silver) — Gemian is intact —
  and fall back to stock-kernel captures per the checklist's abort paths.
- **Note:** Part 2 leaves the **capture kernel** in boot3. Part 3 overwrites
  boot3 with the Experimental kernel, so this ordering is deliberate: capture
  first, then repurpose boot3.

## Part 3 — flash the Experimental slot

Two writes, both targeted, neither touching the GPT / boot2 / nvram / the
Gemian rootfs:

### 3a. Experimental kernel → boot3

- [ ] (Recommended) read-back the current boot3 first if you did not in Part 1.
- [ ] Enter download mode; flash the packed boot3 image:
      ```sh
      mtk w boot3 release/out/boot3-experimental.img   # boot3 ONLY, single partition
      ```
- [ ] Verify: read boot3 back and compare SHA-256 against
      `MANIFEST.txt`'s `boot3_image`.
- **Risk:** replaces boot3 (already backed up twice). **NOT** boot2, **NOT**
  the GPT (`mtk w <partname>`, never `mtk wl`). **Abort/recovery:** flash fails
  / hangs → power off, **Esc + silver**, retry; if the new kernel will not boot,
  restore boot3 from the TWRP backup or the Part-1 read-back and boot Gemian
  (silver) to regroup.

### 3b. Rootfs loop image → a FILE on the `linux` partition

- [ ] Boot **boot2** (silver) into Gemian (which mounts the `linux` partition
      read-write as its own rootfs) — or mount the `linux` partition from a
      recovery/adb shell. Copy the image **beside** Gemian's rootfs, exactly
      like the existing `ubuntu.img`:
      ```sh
      # from within Gemian, / is the linux partition's Gemian rootfs:
      cp /path/to/experimental.img /experimental.img
      sync
      sha256sum /experimental.img     # compare to MANIFEST.txt rootfs build id
      ```
- **Risk:** this is a **plain file write** onto an existing filesystem — no
  partition-level operation, no reformat, no resize. The initramfs mounts the
  host partition **read-only with `noload`** while probing (PACKAGING.md §2), so
  a mis-boot cannot corrupt Gemian. **Abort/recovery:** if the copy fails or the
  partition is low on space, delete `/experimental.img`; Gemian is unaffected.
  **Never** `mkfs`/`resize2fs` the `linux` partition.

## Part 4 — first bring-up attempt (supervised)

Boot the Experimental slot: **silver + Esc**. The initramfs finds the `linux`
partition by GPT name, loop-mounts `/experimental.img` `ro,noload`, and pivots
(markers `GEMINI-INITRAMFS-PIVOT-OK` → `GEMINI-ROOTFS-SHELL-OK`). If it drops to
the rescue shell (`GEMINI-INITRAMFS-RESCUE`) it has written nothing — debug the
probe (`gemini.rescue`, `gemini.rootdev=`, `gemini.rootimg=` on the cmdline).

Then bring CONSYS up **step by step, next to the Reference-slot captures**. The
daemons ship **inert on purpose** (nothing powers CONSYS as a boot side effect —
that would race and contaminate the first capture; see `userspace/wmt-daemons`).

### 4a. Load the kernel modules — explicit order, by full path

> **RESOLVED (issue #22).** The former `wmt_chrdev_wifi`↔`wlan_gen3`
> bidirectional `EXPORT_SYMBOL` cycle was fixed by merging the chardev into
> `wlan_gen3` (the `/dev/wmtWifi` node and reset FSM are now intra-module). The
> module set is 3, not 4, and loads in the acyclic order
> `mtk_btif → mtk_stp_wmt_soc → wlan_gen3`; `depmod` emits a valid `modules.dep`
> so `modprobe` also works. `wmt_chrdev_wifi.ko` no longer exists as a separate
> module.
>
> The assembled rootfs already carries the `depmod`-generated
> `/lib/modules/<kver>/modules.dep` (assembly runs `depmod` on the staged
> overlay), and the release CI **proves** `modprobe -n` resolves all three in
> this order before the image is qualified (issue #24). So on-device
> `modprobe wlan_gen3` pulls the whole chain; the explicit `insmod`-by-path
> sequence below is the belt-and-braces equivalent for the first bring-up.

Load the **WMT core** first — this is enough for C2's pre-firmware/first-query
capture and does not depend on the Wi-Fi datapath:

```sh
K=$(ls -d /lib/modules/*/updates | head -1)   # gemini kver dir (not `uname -r`)
insmod "$K/mtk_btif.ko"          # leaf; creates BTIF plumbing
insmod "$K/mtk_stp_wmt_soc.ko"   # needs 9 symbols from mtk_btif; creates /dev/wmtdetect, /dev/stpwmt
ls -l /dev/wmtdetect /dev/stpwmt # expect both present
```

Then the Wi-Fi datapath (single module now; `/dev/wmtWifi` appears on load):
`insmod "$K/wlan_gen3.ko"` (or `modprobe wlan_gen3` — modules.dep is valid).

- **Risk:** loading `mtk_stp_wmt_soc` registers the WMT core but powers nothing
  by itself (the A4 daemon change removes the always-power-on probe; function
  drivers request power via `func_on`). **Abort/recovery:** `rmmod` in reverse
  order; if a module wedges, `sudo reboot` back into boot3 and inspect `dmesg`.

### 4b. Start the daemons under supervision (WMT handshake)

Mirror the vendor order (Android init.rc lineage), watching the journal:

```sh
systemctl start wmt-loader.service      # oneshot: chip-id handshake via /dev/wmtdetect
systemctl start wmt-launcher.service    # long-running: STP config + ROM-patch offer, -p /lib/firmware
journalctl -u wmt-launcher -f
```

Watch for the **WMT handshake**: `wmt_loader` publishing the chip id, then
`wmt_launcher` taking the MT6797 SoC/BTIF path (`STP_BTIF_FULL`), setting STP
mode, and on `srh_patch` offering the ROM patches from `/lib/firmware`
(`ROMv3_patch_1_0_hdr.bin`, `ROMv3_patch_1_1_hdr.bin`), after which the kernel
`request_firmware()`s them and pushes to the CONSYS MCU.

- **Do NOT `systemctl enable`** the units — keep them supervised/manual this
  session. Only after Internal Wi-Fi works and the sequence is trusted does a
  later slice decide to enable them.
- **Risk:** this is the first time the ported stack powers CONSYS. It cannot
  touch boot2/nvram. **Abort/recovery:** the launcher bounds its waits (A1: 10 s
  default, `WMT_DEV_WAIT_SEC`) and the patch search times out (~2000 ms), so a
  missing device node or bad payload fails cleanly rather than hanging;
  `systemctl stop wmt-launcher wmt-loader`, `rmmod`, reboot boot3 if wedged.

### 4c. If the handshake stalls → the divergence-debug plan

Do **not** start open-ended poking. Enter `04-docs/DIVERGENCE-DEBUG-PLAN.md` at
its decision tree:

- **Gate:** does the ported stack pass `WMT_QUERY_STP`?
  - **PASS** → proceed to patch push, then Wi-Fi function-on; on later failure
    re-enter the tree at the failing step with **C1** as reference.
  - **FAIL** → this is exactly the C2 capture the plan calls for. Capture the
    enumerated register set (CPUPCR `0x18070160`, `SPM_CONN_PWR_CON`
    `0x1000632C`, the BTIF block at `0x1100c000`, …) at the defined sequence
    points, then **diff C2 against C0a in bring-up order** (power → clocks →
    reset → BTIF init → first TX → first RX). The **first divergence is the
    finding**; fix that one item, rebuild, re-run (single-variable discipline).
- Capture everything to `04-docs/captures/<date>/`, **scrub device-unique IDs**,
  commit text only.

---

## Session outputs checklist

- [ ] boot3 backed up two ways (TWRP backup confirmed + fresh read-back).
- [ ] Reference-slot captures C0a / C0b / C1 taken, scrubbed, committed.
- [ ] boot3 = Experimental kernel (read-back SHA-256 matches `MANIFEST.txt`).
- [ ] `/experimental.img` copied onto the `linux` partition beside Gemian
      (SHA-256 matches manifest); Gemian rootfs untouched.
- [ ] Experimental slot boots to a shell (pivot markers seen).
- [ ] WMT core modules load (`mtk_btif`, `mtk_stp_wmt_soc`); `/dev/wmtdetect`
      + `/dev/stpwmt` present.
- [ ] Datapath-module cycle (4a) status recorded — blocked pending the Gen3
      follow-up fix, or fixed and both loaded.
- [ ] First bring-up result recorded: `WMT_QUERY_STP` PASS/FAIL, and if FAIL
      the C2 capture + first-divergence finding.
- [ ] `04-docs/STATE-<date>.md` updated with the new device state.

**Everything above remains NOT-YET-FLASHED until the gate at the top is lifted.**
