#!/bin/bash
# inject-firmware.sh <experimental.img> — replace the 4 placeholder CONSYS
# firmware blobs in a CI experimental.img with the real catalog blobs, in
# place, via debugfs (no loop mount). Verifies each on read-back. Same process
# proven 2026-08-17 morning.
set -euo pipefail
IMG="${1:?usage: inject-firmware.sh <experimental.img>}"
FW=/mercury/data/projects/gemini_linux/05-gemini-payload/consys/rootfs-overlay/lib/firmware
declare -A WANT=(
  [ROMv3_patch_1_0_hdr.bin]=d85864206b999501568a69edd8305319f9961fb65d56f77f438be29ff05c57a8
  [ROMv3_patch_1_1_hdr.bin]=8982bba207df136fe56be1b9d5f394d89128b3fd41977a8741928fe105399784
  [WIFI_RAM_CODE_6797]=c28c50efd411c591372b3a57a46cb99709db56e6cd86d65022af2baa88d840a6
  [WMT_SOC.cfg]=f4a59b622a4e0c1470e475ce33f3edae43b27f1fbdeba54dc7cf07503d132880
)
for f in "${!WANT[@]}"; do
  debugfs -w -R "rm /lib/firmware/$f" "$IMG" 2>/dev/null | grep -v '^debugfs' || true
done
e2fsck -fy "$IMG" >/dev/null 2>&1 || true
for f in "${!WANT[@]}"; do
  debugfs -w -R "write $FW/$f /lib/firmware/$f" "$IMG" 2>/dev/null | grep -v '^debugfs' || true
  debugfs -w -R "sif /lib/firmware/$f uid 0" "$IMG" >/dev/null 2>&1
  debugfs -w -R "sif /lib/firmware/$f gid 0" "$IMG" >/dev/null 2>&1
  debugfs -w -R "sif /lib/firmware/$f mode 0100644" "$IMG" >/dev/null 2>&1
done
e2fsck -fy "$IMG" >/dev/null 2>&1 || true
e2fsck -fn "$IMG" >/dev/null && echo "fsck clean"
T=$(mktemp -d)
ok=1
for f in "${!WANT[@]}"; do
  debugfs -R "dump /lib/firmware/$f $T/$f" "$IMG" 2>/dev/null
  got=$(sha256sum "$T/$f" | cut -d' ' -f1)
  if [ "$got" = "${WANT[$f]}" ]; then echo "OK  $f"; else echo "MISMATCH $f: $got"; ok=0; fi
done
rm -rf "$T"
[ "$ok" = 1 ] && echo "ALL FIRMWARE VERIFIED  ->  $(sha256sum "$IMG")" || { echo "FIRMWARE INJECTION FAILED"; exit 1; }
