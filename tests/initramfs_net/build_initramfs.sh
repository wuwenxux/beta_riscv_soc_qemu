#!/usr/bin/env bash
# =============================================================================
# tests/initramfs_net/build_initramfs.sh
#
# Builds a net-testing initramfs by injecting /init_net and /etc/udhcpc.sh
# into an existing base initramfs (which already contains the RISC-V userland
# binaries: busybox, ip, udhcpc, ping, sh).
#
# Usage:
#   build_initramfs.sh --base <base.cpio.gz> \
#                      --out  <output.cpio.gz> \
#                      --init-src <init_script> \
#                      --udhcpc <udhcpc_handler>
#
# The output initramfs is functionally identical to the base, with two extra
# files added:
#   /init_net         — the network test init script (chmod 755)
#   /etc/udhcpc.sh    — the DHCP lease handler       (chmod 755)
#
# Requirements (all available by default on macOS):
#   cpio, gzip, zcat
# =============================================================================

set -euo pipefail

# ── Argument parsing ──────────────────────────────────────────────────────
BASE_INITRD=""
OUT_INITRD=""
INIT_SRC=""
UDHCPC_SRC=""

while [ $# -gt 0 ]; do
    case "$1" in
        --base)     BASE_INITRD="$2"; shift 2 ;;
        --out)      OUT_INITRD="$2";  shift 2 ;;
        --init-src) INIT_SRC="$2";    shift 2 ;;
        --udhcpc)   UDHCPC_SRC="$2";  shift 2 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

# ── Validate inputs ───────────────────────────────────────────────────────
_die() { echo "ERROR: $*" >&2; exit 1; }

[ -n "$BASE_INITRD" ] || _die "--base is required"
[ -n "$OUT_INITRD"  ] || _die "--out is required"
[ -n "$INIT_SRC"    ] || _die "--init-src is required"
[ -n "$UDHCPC_SRC"  ] || _die "--udhcpc is required"

[ -f "$BASE_INITRD" ] || _die "Base initramfs not found: $BASE_INITRD"
[ -f "$INIT_SRC"    ] || _die "init script not found: $INIT_SRC"
[ -f "$UDHCPC_SRC"  ] || _die "udhcpc script not found: $UDHCPC_SRC"

# ── Check required tools ──────────────────────────────────────────────────
for cmd in cpio gzip; do
    command -v "$cmd" >/dev/null 2>&1 || _die "Required tool not found: $cmd"
done

# ── Check if rebuild is needed ────────────────────────────────────────────
if [ -f "$OUT_INITRD" ]; then
    # Rebuild if any source is newer than the output
    if [ "$BASE_INITRD" -nt "$OUT_INITRD" ] || \
       [ "$INIT_SRC"    -nt "$OUT_INITRD" ] || \
       [ "$UDHCPC_SRC"  -nt "$OUT_INITRD" ]; then
        echo "[build_initramfs] Sources changed — rebuilding $OUT_INITRD"
    else
        echo "[build_initramfs] $OUT_INITRD is up-to-date, skipping rebuild"
        exit 0
    fi
fi

# ── Create a temporary work directory ────────────────────────────────────
WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/beta_initramfs_net_XXXXXX")
trap 'rm -rf "$WORKDIR"' EXIT

echo "[build_initramfs] Working directory: $WORKDIR"

# ── Extract base initramfs ────────────────────────────────────────────────
echo "[build_initramfs] Extracting: $BASE_INITRD"
(
    cd "$WORKDIR"
    gzip -cd "$BASE_INITRD" | cpio -idm 2>/dev/null
)

BASE_SIZE=$(du -sh "$WORKDIR" 2>/dev/null | cut -f1)
echo "[build_initramfs] Base extracted: ~$BASE_SIZE"

# ── Inject network init script ────────────────────────────────────────────
cp "$INIT_SRC" "${WORKDIR}/init_net"
chmod 755 "${WORKDIR}/init_net"
echo "[build_initramfs] Added /init_net"

# ── Inject udhcpc handler ────────────────────────────────────────────────
mkdir -p "${WORKDIR}/etc"
cp "$UDHCPC_SRC" "${WORKDIR}/etc/udhcpc.sh"
chmod 755 "${WORKDIR}/etc/udhcpc.sh"
echo "[build_initramfs] Added /etc/udhcpc.sh"

# ── Ensure /etc/resolv.conf exists (writable placeholder) ────────────────
if [ ! -f "${WORKDIR}/etc/resolv.conf" ]; then
    touch "${WORKDIR}/etc/resolv.conf"
    echo "[build_initramfs] Created empty /etc/resolv.conf"
fi

# ── Sanity check: verify busybox-provided tools exist in rootfs ──────────
_warn_missing() {
    local f="$1"
    if [ ! -e "${WORKDIR}${f}" ]; then
        echo "[build_initramfs] WARNING: $f not found in base rootfs" \
             "— Round 3 network test may fail" >&2
    fi
}
_warn_missing "/bin/sh"
_warn_missing "/sbin/ip"
_warn_missing "/sbin/udhcpc"
_warn_missing "/bin/ping"

# ── Repack ────────────────────────────────────────────────────────────────
echo "[build_initramfs] Repacking → $OUT_INITRD"
mkdir -p "$(dirname "$OUT_INITRD")"

(
    cd "$WORKDIR"
    find . -depth | sort | cpio -H newc -o 2>/dev/null
) | gzip -9 > "$OUT_INITRD"

OUT_SIZE=$(wc -c < "$OUT_INITRD")
echo "[build_initramfs] Done. Output: $OUT_INITRD (${OUT_SIZE} bytes)"
