#!/bin/sh
# capture-lib.sh -- shared helpers for the Reference-slot capture scripts
# (gemini_linux tracker issue #9; method: 04-docs/DIVERGENCE-DEBUG-PLAN.md).
#
# Runs ON THE DEVICE (Gemian, Debian 9 stretch, as root over SSH) during
# the first physical session. Read-only with respect to device state
# except where a capture script explicitly performs a plan-sanctioned
# action (C0b func-on trigger, C1 daemon release).
#
# Register list and gating rules mirror the instrumented kernel's
# wmt_capture_regtable() (capture-kit patch 0002) and the mirrored bsg100
# consys-golden-harvest.sh:
#   - CONN-domain regs (0x1807xxxx) only while SPM_PWR_STATUS/2ND bit 1
#     are set (DEVAPC violation / bus hang otherwise);
#   - BTIF block (0x1100Cxxx) only while CLK_INFRA_BTIF is ungated
#     (INFRA_CG_STA0 0x10001090 bit 31 == 0);
#   - APDMA BTIF channels only while CLK_INFRA_AP_DMA is ungated
#     (INFRA_CG_STA1 0x10001094 bit 18 == 0);
#   - PMIC regs are NOT memory-mapped: they come from the kernel hook
#     (pwrap) only -- devmem cannot reach them.
#
# Output: one "CAPTURE-REG: <seq-point> <NAME> <addr> = <value>" line per
# register per sequence point (the plan's field-per-line table), written
# to $CAPDIR. Kernel-side CAPTURE-* lines land in dmesg and are harvested
# by the same scripts.

CAPROOT=${CAPROOT:-/home/gemini/captures}
CAPHOOK=/proc/driver/wmt_capture
# shellcheck disable=SC2034  # used by the sourcing capture scripts
WMTDBG=/proc/driver/wmt_dbg

cap_init() { # cap_init <capture-name>
    CAPDIR="$CAPROOT/$(date +%Y-%m-%d)/$1-$(date +%H%M%S)"
    mkdir -p "$CAPDIR" || { echo "FATAL: cannot create $CAPDIR"; exit 1; }
    echo "capture dir: $CAPDIR"
    uname -a > "$CAPDIR/uname.txt"
    date -u +"%Y-%m-%dT%H:%M:%SZ" > "$CAPDIR/timestamp.txt"
}

# ---- devmem ---------------------------------------------------------
DEVMEM=""
devmem_init() {
    for c in devmem devmem2 "busybox devmem"; do
        if $c 0x10006180 >/dev/null 2>&1; then DEVMEM="$c"; return 0; fi
    done
    echo "WARNING: no working devmem/devmem2/busybox devmem - userspace register reads disabled"
    return 1
}

dm() { # dm <addr>  -> prints value or READ-FAILED
    [ -n "$DEVMEM" ] || { echo "READ-FAILED"; return 1; }
    $DEVMEM "$1" 2>/dev/null || echo "READ-FAILED"
}

emit() { # emit <seqpoint> <name> <addr> <value>
    printf 'CAPTURE-REG: %s %s %s = %s\n' "$1" "$2" "$3" "$4"
}

rdreg() { # rdreg <seqpoint> <name> <addr>
    emit "$1" "$2" "$3" "$(dm "$3")"
}

conn_powered() { # SPM_PWR_STATUS bit1 && 2ND bit1
    s=$(dm 0x10006180); s2=$(dm 0x10006184)
    case "$s" in READ-FAILED) return 1;; esac
    case "$s2" in READ-FAILED) return 1;; esac
    [ $(( s & 2 )) -ne 0 ] && [ $(( s2 & 2 )) -ne 0 ]
}

btif_ungated() { # INFRA_CG_STA0 bit31 clear (bit set = gated)
    v=$(dm 0x10001090)
    case "$v" in READ-FAILED) return 1;; esac
    [ $(( (v >> 31) & 1 )) -eq 0 ]
}

apdma_ungated() { # INFRA_CG_STA1 bit18 clear
    v=$(dm 0x10001094)
    case "$v" in READ-FAILED) return 1;; esac
    [ $(( (v >> 18) & 1 )) -eq 0 ]
}

# ---- full register table (devmem view) ------------------------------
# Mirrors wmt_capture_regtable() row-for-row; PMIC rows are delegated to
# the kernel hook.
regtable() { # regtable <seqpoint>
    sp=$1
    # AP-side (always safe)
    rdreg "$sp" SPM_PWRON_CONFG_EN      0x10006000
    rdreg "$sp" SPM_PWR_STATUS          0x10006180
    rdreg "$sp" SPM_PWR_STATUS_2ND      0x10006184
    rdreg "$sp" SPM_CONN_PWR_CON        0x1000632C
    rdreg "$sp" SPM_CONN_PWR_CON_STALE  0x10006280
    rdreg "$sp" INFRA_TOPAXI_PROT_EN    0x10001220
    rdreg "$sp" INFRA_TOPAXI_PROT_STA1  0x10001228
    rdreg "$sp" AP2CONN_OSC_EN          0x10001F00
    rdreg "$sp" CONSYS_EMI_MAPPING      0x10001340
    rdreg "$sp" CONN2AP_SLEEP_MASK      0x10001350
    rdreg "$sp" WDT_SWSYSRST            0x10007018
    rdreg "$sp" INFRA_CG_STA0           0x10001090
    rdreg "$sp" INFRA_CG_STA1           0x10001094
    rdreg "$sp" PWRAP_DCXO_ENABLE       0x1000D18C
    rdreg "$sp" PWRAP_DCXO_CONN_ADR0    0x1000D190
    rdreg "$sp" PWRAP_DCXO_CONN_WDATA0  0x1000D194
    rdreg "$sp" PWRAP_DCXO_CONN_ADR1    0x1000D198
    rdreg "$sp" PWRAP_DCXO_CONN_WDATA1  0x1000D19C

    # CONN domain (gated)
    if conn_powered; then
        emit "$sp" CONN_POWERED - 1
        rdreg "$sp" CONSYS_HW_VER   0x18070000
        rdreg "$sp" CONSYS_FW_VER   0x18070004
        rdreg "$sp" CONSYS_CHIP_ID  0x18070008
        rdreg "$sp" MCU_CFG_ACR     0x18070110
        rdreg "$sp" CONSYS_CPUPCR   0x18070160
    else
        emit "$sp" CONN_POWERED - 0
        for r in "CONSYS_HW_VER 0x18070000" "CONSYS_FW_VER 0x18070004" \
                 "CONSYS_CHIP_ID 0x18070008" "MCU_CFG_ACR 0x18070110" \
                 "CONSYS_CPUPCR 0x18070160"; do
            # shellcheck disable=SC2086  # intentional word split
            set -- $r; emit "$sp" "$1" "$2" SKIPPED-CONN-UNPOWERED
        done
    fi

    # BTIF host block (gated on INFRA_BTIF clock)
    if btif_ungated; then
        rdreg "$sp" BTIF_IER           0x1100C004
        rdreg "$sp" BTIF_IIR_FIFOCTRL  0x1100C008
        rdreg "$sp" BTIF_FAKELCR       0x1100C00C
        rdreg "$sp" BTIF_LSR           0x1100C014
        rdreg "$sp" BTIF_SLEEP_EN      0x1100C048
        rdreg "$sp" BTIF_DMA_EN        0x1100C04C
        rdreg "$sp" BTIF_RTOCNT        0x1100C054
        rdreg "$sp" BTIF_TRI_LVL       0x1100C060
        rdreg "$sp" BTIF_WAK           0x1100C064
        rdreg "$sp" BTIF_WAT_TIME      0x1100C068
        rdreg "$sp" BTIF_HANDSHAKE     0x1100C06C
    else
        for r in "BTIF_IER 0x1100C004" "BTIF_IIR_FIFOCTRL 0x1100C008" \
                 "BTIF_FAKELCR 0x1100C00C" "BTIF_LSR 0x1100C014" \
                 "BTIF_SLEEP_EN 0x1100C048" "BTIF_DMA_EN 0x1100C04C" \
                 "BTIF_RTOCNT 0x1100C054" "BTIF_TRI_LVL 0x1100C060" \
                 "BTIF_WAK 0x1100C064" "BTIF_WAT_TIME 0x1100C068" \
                 "BTIF_HANDSHAKE 0x1100C06C"; do
            # shellcheck disable=SC2086  # intentional word split
            set -- $r; emit "$sp" "$1" "$2" SKIPPED-BTIF-CLK-GATED
        done
    fi

    # APDMA BTIF channels (gated on INFRA_AP_DMA clock)
    if apdma_ungated; then
        rdreg "$sp" APDMA_BTIF_TX_INT_EN 0x11000884
        rdreg "$sp" APDMA_BTIF_TX_EN     0x11000888
        rdreg "$sp" APDMA_BTIF_RX_INT_EN 0x11000904
        rdreg "$sp" APDMA_BTIF_RX_EN     0x11000908
    else
        for r in "APDMA_BTIF_TX_INT_EN 0x11000884" "APDMA_BTIF_TX_EN 0x11000888" \
                 "APDMA_BTIF_RX_INT_EN 0x11000904" "APDMA_BTIF_RX_EN 0x11000908"; do
            # shellcheck disable=SC2086  # intentional word split
            set -- $r; emit "$sp" "$1" "$2" SKIPPED-APDMA-CLK-GATED
        done
    fi

    # PMIC rows: pwrap only reachable from kernel space
    for r in "PMIC_DCXO_CW00 0x7000" "PMIC_LDO_VCN18_CON0 0x0A52" \
             "PMIC_LDO_VCN28_CON0 0x0A0C" "PMIC_LDO_VCN33_CON0 0x0A92"; do
        # shellcheck disable=SC2086  # intentional word split
        set -- $r
        if [ -w "$CAPHOOK" ]; then
            emit "$sp" "$1" "$2" SEE-KERNEL-SNAP   # kernel hook emits CAPTURE-PMIC into dmesg
        else
            emit "$sp" "$1" "$2" SKIPPED-NEEDS-KERNEL-HOOK
        fi
    done

    # EMI ctrl window, first 64 bytes (word reads; full-window CRC comes
    # from the kernel hook). Remap word: bits[11:0] = phys>>20.
    emimap=$(dm 0x10001340)
    case "$emimap" in
        READ-FAILED) emit "$sp" EMI_CTRL_WIN - SKIPPED-EMIMAP-UNREADABLE ;;
        *)
            base=$(( (emimap & 0xFFF) << 20 ))
            if [ "$base" -gt 0 ]; then
                o=0
                while [ $o -lt 64 ]; do
                    emit "$sp" "EMI_CTRL+0x$(printf %x $o)" \
                         "0x$(printf %x $((base + 0x80000 + o)))" \
                         "$(dm $((base + 0x80000 + o)))"
                    o=$((o + 4))
                done
            else
                emit "$sp" EMI_CTRL_WIN - SKIPPED-EMI-REMAP-NOT-PROGRAMMED
            fi
            ;;
    esac
}

# ---- CPUPCR burst (>=32 samples, ~1ms) -------------------------------
# Prefers the kernel hook (true 1ms spacing, in-kernel); falls back to a
# devmem loop (spacing then dominated by process spawn, recorded as-is).
cpupcr_burst() { # cpupcr_burst <seqpoint>
    sp=$1
    if [ -w "$CAPHOOK" ]; then
        echo "cpupcr $sp" > "$CAPHOOK"
        echo "CAPTURE-CPUPCR: $sp = EMITTED-TO-DMESG (kernel hook, 32 samples @1ms)"
        return
    fi
    if ! conn_powered; then
        echo "CAPTURE-CPUPCR: $sp = SKIPPED-CONN-UNPOWERED"
        return
    fi
    i=0
    while [ $i -lt 32 ]; do
        printf 'CAPTURE-CPUPCR: %s sample=%02d = %s\n' "$sp" "$i" "$(dm 0x18070160)"
        i=$((i + 1))
    done
}

# ---- kernel-hook snapshot -------------------------------------------
ksnap() { # ksnap <seqpoint>  (emits into dmesg; harvested later)
    if [ -w "$CAPHOOK" ]; then
        echo "snap $1" > "$CAPHOOK"
        echo "kernel snap '$1' requested (lands in dmesg)"
    else
        echo "kernel snap '$1' UNAVAILABLE (stock kernel? $CAPHOOK missing)"
    fi
}

# ---- daemon state ----------------------------------------------------
# Gemian starts the vendor connectivity daemons (wmt_loader,
# wmt_launcher, ...) inside the Android LXC container:
#   multi-user.target -> lxc@android.service (+ droid-hal-init.service)
#   -> Android init from /system/boot/android-ramdisk.img -> wmt daemons
# (evidence: /etc/systemd/system/multi-user.target.wants/ and
# /var/lib/lxc/android/config in the Gemian rootfs, inspected 2026-08-12
# from the local flash-set linux.img). Holding these two units off holds
# off every WMT userspace actor; the kernel side is additionally inert
# because MTK_WCN_REMOVE_KERNEL_MODULE defers all connectivity driver
# init to wmt_loader's ioctl (connectivity/Makefile, wmt_detect.c).
HOLDOFF_UNITS="lxc@android.service droid-hal-init.service"

daemons_held() {
    for u in $HOLDOFF_UNITS; do
        if systemctl is-active --quiet "$u"; then return 1; fi
    done
    # belt and braces: no WMT device nodes / processes
    [ ! -e /dev/stpwmt ] || return 1
    ! pgrep -f 'wmt_loader|wmt_launcher' >/dev/null 2>&1
}

require_daemons_held() {
    if ! daemons_held; then
        echo "FATAL: WMT daemons / Android LXC active - this capture needs them held off."
        echo "  hold-off: systemctl mask $HOLDOFF_UNITS && reboot"
        exit 1
    fi
}

dmesg_save() { # dmesg_save <file>
    dmesg > "$CAPDIR/$1"
    echo "saved dmesg -> $CAPDIR/$1"
}
