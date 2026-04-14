#!/usr/bin/env bash
# =============================================================================
# Beta SoC QEMU Regression Test Suite
# =============================================================================
#
# USAGE
#   ./regression.sh --all                  Run all 5 rounds
#   ./regression.sh --test smoke           Run Round 1 only
#   ./regression.sh --test smp4            Run Round 2 only
#   ./regression.sh --test virtio          Run Round 3 only
#   ./regression.sh --test smp32           Run Round 4 only
#   ./regression.sh --test irq_check       Run Round 5 only
#   ./regression.sh --list                 Show available rounds
#   ./regression.sh --build-only           Build QEMU, exit
#   ./regression.sh --no-build --all       Skip build step, run all
#
# ROUNDS
#   smoke      Round 1 · QEMU DTB dump — no kernel (30 s)
#   smp4       Round 2 · 4-core SMP Linux boot     (120 s)
#   virtio     Round 3 · PCIe + VirtIO network      (180 s)
#   smp32      Round 4 · Full 32-core production    (600 s)
#   irq_check  Round 5 · IRQ table consistency      (30 s, requires dtc)
#
# EXIT CODES
#   0   All executed rounds passed (or skipped)
#   1   One or more rounds failed
#   2   Fatal error (missing prerequisites, build failed)
#
# REQUIREMENTS (macOS M4 / Apple Silicon)
#   Standard: bash, cpio, gzip, make, strings
#   Optional: brew install dtc          (required for Round 5)
#             brew install coreutils    (provides gtimeout; perl fallback used otherwise)
#
# LOGS
#   Saved to tests/logs/<timestamp>/  — one file per round.
#   The directory persists across runs; old logs are never deleted.
#
# =============================================================================

set -euo pipefail

# ── Self-location ────────────────────────────────────────────────────────
TESTS_DIR="$(cd "$(dirname "$0")" && pwd)"
BASE_DIR="$(cd "${TESTS_DIR}/.." && pwd)"

# ── Run timestamp ────────────────────────────────────────────────────────
RUN_TS="$(date '+%Y%m%d_%H%M%S')"
LOG_DIR="${TESTS_DIR}/logs/${RUN_TS}"

# ── Firmware / binary paths ───────────────────────────────────────────────
QEMU_SRC_DIR="${HOME}/qemu-beta-src"
QEMU_BIN="${QEMU_SRC_DIR}/build/qemu-system-riscv64-unsigned"
CFG="${BASE_DIR}/config/beta_production_config.json"
BIOS="${BASE_DIR}/firmware/bootrom/mcpu_bootrom_production.bin"
OPENSBI="${BASE_DIR}/firmware/opensbi/fw_jump_0x4000000000.bin"
KERNEL="${BASE_DIR}/kernel/Image-6.19.5-beta"
INITRD="${BASE_DIR}/rootfs/initramfs.cpio.gz"

# ── Source shared library ────────────────────────────────────────────────
# shellcheck source=lib/common.sh
source "${TESTS_DIR}/lib/common.sh"

# ── Source test case definitions ─────────────────────────────────────────
for _case_file in "${TESTS_DIR}/cases/"[0-9][0-9]_*.sh; do
    # shellcheck disable=SC1090
    source "$_case_file"
done

# ── Option defaults ───────────────────────────────────────────────────────
DO_BUILD=1
TEST_FILTER="all"
FAIL_FAST=0

# ── Argument parsing ──────────────────────────────────────────────────────
usage() {
    sed -n '/^# USAGE/,/^# =/p' "$0" | sed 's/^# \?//'
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        --all)         TEST_FILTER="all"   ; shift ;;
        --test)        TEST_FILTER="$2"    ; shift 2 ;;
        --list)        _list_tests         ; exit 0 ;;
        --build-only)  DO_BUILD=1; TEST_FILTER="none"; shift ;;
        --no-build)    DO_BUILD=0          ; shift ;;
        --fail-fast)   FAIL_FAST=1         ; shift ;;
        --help|-h)     usage ;;
        *) echo "Unknown option: $1  (try --help)" >&2; exit 2 ;;
    esac
done

_list_tests() {
    echo "Available test rounds:"
    echo "  smoke      Round 1 · Smoke (DTB dump, no kernel)"
    echo "  smp4       Round 2 · 4-core SMP boot"
    echo "  virtio     Round 3 · VirtIO network data path"
    echo "  smp32      Round 4 · Full 32-core production"
    echo "  irq_check  Round 5 · IRQ table consistency (requires dtc)"
}

# ── Banner ────────────────────────────────────────────────────────────────
_banner() {
    echo ""
    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║          Beta SoC QEMU Regression Test Suite                ║"
    printf "║  Run: %-54s ║\n" "$RUN_TS"
    printf "║  Base: %-53s ║\n" "$BASE_DIR"
    echo "╚══════════════════════════════════════════════════════════════╝"
    echo ""
}

# ── Incremental QEMU build ────────────────────────────────────────────────
_build_qemu() {
    log_hdr "Build Step"

    if [ ! -d "$QEMU_SRC_DIR" ]; then
        log_fail "QEMU source directory not found: $QEMU_SRC_DIR"
        return 2
    fi

    log_info "Running incremental build in $QEMU_SRC_DIR ..."
    local before_ts=0
    [ -f "$QEMU_BIN" ] && before_ts=$(stat -f '%m' "$QEMU_BIN" 2>/dev/null || \
                                       stat -c '%Y' "$QEMU_BIN" 2>/dev/null || echo 0)

    if ! (cd "$QEMU_SRC_DIR" && make -j"$(sysctl -n hw.logicalcpu 2>/dev/null || nproc 2>/dev/null || echo 4)" 2>&1); then
        log_fail "Build FAILED — see output above"
        return 2
    fi

    local after_ts=0
    [ -f "$QEMU_BIN" ] && after_ts=$(stat -f '%m' "$QEMU_BIN" 2>/dev/null || \
                                      stat -c '%Y' "$QEMU_BIN" 2>/dev/null || echo 0)

    if [ "$after_ts" -gt "$before_ts" ]; then
        log_ok "Build complete — binary updated: $QEMU_BIN"
    else
        log_info "Build complete — binary unchanged (no source changes)"
    fi
    return 0
}

# ── Test dispatcher ────────────────────────────────────────────────────────
_run_one() {
    local name="$1"
    case "$name" in
        smoke)     tc_smoke     ;;
        smp4)      tc_smp4      ;;
        virtio)    tc_virtio_net ;;
        smp32)     tc_smp32     ;;
        irq_check) tc_irq_check ;;
        *)
            log_fail "Unknown test name: $name"
            log_fail "Run  ./regression.sh --list  for valid names"
            return 2
            ;;
    esac
}

_run_all() {
    _run_one smoke     || { [ "$FAIL_FAST" -eq 1 ] && return 1; true; }
    _run_one smp4      || { [ "$FAIL_FAST" -eq 1 ] && return 1; true; }
    _run_one virtio    || { [ "$FAIL_FAST" -eq 1 ] && return 1; true; }
    _run_one smp32     || { [ "$FAIL_FAST" -eq 1 ] && return 1; true; }
    _run_one irq_check || { [ "$FAIL_FAST" -eq 1 ] && return 1; true; }
    return 0
}

# ── Main ──────────────────────────────────────────────────────────────────
main() {
    _banner

    # ── Create log directory ──────────────────────────────────────────────
    mkdir -p "$LOG_DIR"
    log_info "Logs: $LOG_DIR"

    # ── Build step ────────────────────────────────────────────────────────
    if [ "$DO_BUILD" -eq 1 ]; then
        _build_qemu || exit 2
    fi

    # Exit early if build-only
    [ "$TEST_FILTER" = "none" ] && exit 0

    # ── Prerequisites ─────────────────────────────────────────────────────
    if ! check_prerequisites; then
        log_fail "Fatal prerequisite failure — aborting"
        exit 2
    fi

    # ── Run tests ─────────────────────────────────────────────────────────
    log_info "Starting test run: filter='$TEST_FILTER'"

    if [ "$TEST_FILTER" = "all" ]; then
        _run_all || true   # collect all results even on failure
    else
        _run_one "$TEST_FILTER" || true
    fi

    # ── Final summary ─────────────────────────────────────────────────────
    # Write summary to log file as well
    {
        echo "=== Beta SoC QEMU Regression — Run $RUN_TS ==="
        echo "Filter: $TEST_FILTER"
        echo ""
        for (( i=0; i<${#_TC_NAMES[@]}; i++ )); do
            printf "%-8s  %-20s  %s\n" \
                "${_TC_STATUSES[$i]}" "${_TC_NAMES[$i]}" "${_TC_REASONS[$i]}"
        done
        echo ""
        printf "Passed: %d  Failed: %d  Skipped: %d\n" \
            "$PASS_COUNT" "$FAIL_COUNT" "$SKIP_COUNT"
    } > "${LOG_DIR}/summary.txt"

    print_summary
    local exit_code=$?

    log_info "Log directory: $LOG_DIR"
    log_info "Summary:       ${LOG_DIR}/summary.txt"

    exit $exit_code
}

main "$@"
