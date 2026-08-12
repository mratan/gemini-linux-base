#!/bin/sh
# scrub-capture.sh -- scrub device-unique identifiers from capture files
# before they are committed (standing never-commit rule: boot logs and
# register dumps can carry IMEIs and MAC addresses).
#
# Rewrites files IN PLACE (keep the raw originals on the retrieval
# workstation outside git):
#   - MAC addresses        -> XX:XX:XX:XX:XX:XX
#   - 15-digit IMEI-shaped -> IMEI-SCRUBBED
#   - ESSID strings in iw/iwconfig output -> ESSID-SCRUBBED
#
# Usage: sh scrub-capture.sh <file...>

set -u
[ $# -ge 1 ] || { echo "usage: $0 <file...>" >&2; exit 2; }

for f in "$@"; do
    [ -f "$f" ] || { echo "skip (not a file): $f"; continue; }
    sed -E -i \
        -e 's/([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}/XX:XX:XX:XX:XX:XX/g' \
        -e 's/\b[0-9]{15}\b/IMEI-SCRUBBED/g' \
        -e 's/(ESSID[:=])"[^"]*"/\1"ESSID-SCRUBBED"/g' \
        -e 's/(SSID: ).*/\1ESSID-SCRUBBED/g' \
        "$f"
    echo "scrubbed: $f"
done
echo "Reminder: verify with 'grep -iE \"imei|([0-9a-f]{2}:){5}\" <files>' before commit."
