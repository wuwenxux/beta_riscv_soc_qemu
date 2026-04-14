#!/usr/bin/env bash
# =============================================================================
# tests/lib/common.sh — shared utilities for Beta SoC QEMU regression suite
#
# Expected globals (set by regression.sh before sourcing):
#   TESTS_DIR, BASE_DIR, LOG_DIR, QEMU_BIN
#   CFG, BIOS, OPENSBI, KERNEL, INITRD
#   RUN_TS  (timestamp string for this run)
# =============================================================================

# ── Terminal colours (disabled when not a tty) ────────────────────────────
if [ -t 1 ]; then
    RED='\033[0;31m'; GRN='\033[0;32m'; YLW='\033[0;33m'
    BLU='\033[0;34m'; CYN='\033[0;36m'; BLD='\033[1m'; RST='\033[0m'
else
    RED=''; GRN=''; YLW=''; BLU=''; CYN=''; BLD=''; RST=''
fi

# ── Logging helpers ───────────────────────────────────────────────────────
_ts()      { date '+%H:%M:%S'; }
log_info() { printf "${BLU}[%s INFO ]${RST}  %s\n" "$(_ts)" "$*"; }
log_ok()   { printf "${GRN}[%s PASS ]${RST}  %s\n" "$(_ts)" "$*"; }
log_warn() { printf "${YLW}[%s WARN ]${RST}  %s\n" "$(_ts)" "$*"; }
log_fail() { printf "${RED}[%s FAIL ]${RST}  %s\n" "$(_ts)" "$*"; }
log_step() { printf "${CYN}[%s  -- ]${RST}  %s\n"  "$(_ts)" "$*"; }
log_hdr()  { printf "\n${BLD}══ %s ══${RST}\n" "$*"; }

# ── macOS-compatible timeout wrapper ─────────────────────────────────────
# Usage: timeout_run <seconds> <cmd> [args...]
# Returns: 0=ok  124=timed-out  127=not-found  else=cmd-exit-code
timeout_run() {
    local secs="$1"; shift
    # Prefer GNU coreutils gtimeout, then BSD timeout, then perl fallback
    if command -v gtimeout >/dev/null 2>&1; then
        gtimeout "$secs" "$@"
        return $?
    fi
    if command -v timeout >/dev/null 2>&1; then
        timeout "$secs" "$@"
        return $?
    fi
    # Pure-perl fallback — no Homebrew dependency required
    _TIMEOUT_SECS="$secs" perl -MPOSIX -e '
        my $pid = fork // die "fork: $!\n";
        if ($pid == 0) { exec @ARGV; exit 127 }
        local $SIG{ALRM} = sub {
            kill TERM => $pid;
            sleep 1;
            kill KILL => $pid if kill 0 => $pid;
            exit 124;
        };
        alarm $ENV{_TIMEOUT_SECS};
        waitpid($pid, 0);
        alarm 0;
        exit(($? & 127) ? 128 + ($? & 127) : $? >> 8);
    ' -- "$@"
}

# ── QEMU process management ───────────────────────────────────────────────

# Start QEMU in background; prints PID to stdout.
# Usage: qpid=$(qemu_start <logfile> [qemu-args...])
qemu_start() {
    local logfile="$1"; shift
    "$QEMU_BIN" "$@" -nographic > "$logfile" 2>&1 &
    echo $!
}

# Gracefully stop a QEMU instance.
qemu_stop() {
    local pid="${1:-}"
    [ -z "$pid" ] && return
    kill "$pid" 2>/dev/null
    # Give it 2 s to die cleanly before SIGKILL
    local i=0
    while kill -0 "$pid" 2>/dev/null && [ $i -lt 20 ]; do
        sleep 0.1; i=$((i+1))
    done
    kill -9 "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    true
}

# ── Log pattern matching ──────────────────────────────────────────────────
# QEMU serial output may contain control chars → use `strings` to sanitise.

# Poll a log file for an ERE pattern until found or timeout.
# Usage: wait_for_pattern <logfile> <pattern> <timeout_secs> [qemu_pid]
# Returns: 0=found  1=qemu-died-first  124=timeout
wait_for_pattern() {
    local logfile="$1"
    local pattern="$2"
    local deadline=$(( $(date +%s) + $3 ))
    local qpid="${4:-}"

    while [ "$(date +%s)" -lt "$deadline" ]; do
        # Fast path: pattern already in log
        if [ -f "$logfile" ] && strings "$logfile" 2>/dev/null | grep -qE "$pattern"; then
            return 0
        fi
        # If QEMU died, do one final check then give up
        if [ -n "$qpid" ] && ! kill -0 "$qpid" 2>/dev/null; then
            sleep 0.2
            if strings "$logfile" 2>/dev/null | grep -qE "$pattern"; then
                return 0
            fi
            return 1
        fi
        sleep 0.5
    done
    return 124
}

# One-shot check: does pattern appear in log?
log_contains() { strings "$1" 2>/dev/null | grep -qE "$2"; }

# Extract matching lines from log.
log_extract()  { strings "$1" 2>/dev/null | grep -E  "$2"; }

# Count matching lines.
log_count()    { strings "$1" 2>/dev/null | grep -cE "$2" 2>/dev/null || echo 0; }

# ── Test result tracking (bash 3.2 compatible, no assoc arrays) ───────────
PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0

# Parallel indexed arrays for the result table
_TC_NAMES=()
_TC_STATUSES=()
_TC_REASONS=()

_tc_record() {
    local status="$1" name="$2" reason="${3:-}"
    _TC_NAMES+=("$name")
    _TC_STATUSES+=("$status")
    _TC_REASONS+=("$reason")
}

tc_pass() {
    PASS_COUNT=$((PASS_COUNT+1))
    _tc_record PASS "$1" "${2:-OK}"
    log_ok  "[$1] PASSED${2:+ — $2}"
}

tc_fail() {
    FAIL_COUNT=$((FAIL_COUNT+1))
    _tc_record FAIL "$1" "${2:-unknown failure}"
    log_fail "[$1] FAILED${2:+ — $2}"
}

tc_skip() {
    SKIP_COUNT=$((SKIP_COUNT+1))
    _tc_record SKIP "$1" "${2:-skipped}"
    log_warn "[$1] SKIPPED${2:+ — $2}"
}

# Print the final summary table and return 0=all-pass / 1=any-fail.
print_summary() {
    local total=$(( PASS_COUNT + FAIL_COUNT + SKIP_COUNT ))
    printf "\n"
    printf "╔══════════════════════════════════════════════════════════════╗\n"
    printf "║          Beta SoC QEMU Regression Suite — Results           ║\n"
    printf "╠══════╤══════════════════════╤═══════════════════════════════╣\n"
    printf "║ %-4s │ %-20s │ %-29s ║\n" "Res" "Round" "Details"
    printf "╠══════╪══════════════════════╪═══════════════════════════════╣\n"

    local i
    for (( i=0; i<${#_TC_NAMES[@]}; i++ )); do
        local nm="${_TC_NAMES[$i]}"
        local st="${_TC_STATUSES[$i]}"
        local re="${_TC_REASONS[$i]}"
        # Truncate reason to 29 chars for the table
        re="${re:0:29}"
        case "$st" in
            PASS) printf "║ ${GRN}%-4s${RST} │ %-20s │ %-29s ║\n" "$st" "$nm" "$re" ;;
            FAIL) printf "║ ${RED}%-4s${RST} │ %-20s │ %-29s ║\n" "$st" "$nm" "$re" ;;
            SKIP) printf "║ ${YLW}%-4s${RST} │ %-20s │ %-29s ║\n" "$st" "$nm" "$re" ;;
        esac
    done

    printf "╠══════╧══════════════════════╧═══════════════════════════════╣\n"
    printf "║  Passed: %2d / %2d" "$PASS_COUNT" "$total"
    [ "$SKIP_COUNT" -gt 0 ] && printf "   Skipped: %2d" "$SKIP_COUNT"
    [ "$FAIL_COUNT" -gt 0 ] && printf "   ${RED}Failed:  %2d${RST}" "$FAIL_COUNT"
    printf "\n╚══════════════════════════════════════════════════════════════╝\n\n"

    [ "$FAIL_COUNT" -eq 0 ]   # return 0 iff all pass (or skip)
}

# ── Prerequisites checker ─────────────────────────────────────────────────
check_prerequisites() {
    local fatal=0

    log_hdr "Prerequisites"

    _check_file() {
        local label="$1" path="$2" required="${3:-1}"
        if [ -f "$path" ]; then
            log_step "$label: $path ✓"
        elif [ "$required" = "1" ]; then
            log_fail "$label missing: $path"
            fatal=1
        else
            log_warn "$label not found (optional): $path"
        fi
    }

    _check_cmd() {
        local cmd="$1" hint="${2:-}"
        if command -v "$cmd" >/dev/null 2>&1; then
            log_step "command $cmd: $(command -v "$cmd") ✓"
        else
            log_warn "command $cmd not found${hint:+ — $hint}"
        fi
    }

    _check_file "QEMU binary"    "$QEMU_BIN"
    _check_file "Machine config" "$CFG"
    _check_file "MCPU BootROM"   "$BIOS"
    _check_file "OpenSBI"        "$OPENSBI"
    _check_file "Kernel"         "$KERNEL"
    _check_file "Initramfs"      "$INITRD"

    _check_cmd dtc   "brew install dtc  (required for Round 5)"
    _check_cmd cpio  "should be built-in on macOS"
    _check_cmd gzip  "should be built-in on macOS"

    [ "$fatal" -eq 0 ]
}
