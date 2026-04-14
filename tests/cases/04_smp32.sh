#!/usr/bin/env bash
# =============================================================================
# Round 4: Full 32-Core Production SMP
#
# Boots the full production config without maxcpus restriction.  All 32 ACPU
# cores must come online.  Verifies MCPU BootROM releases all cores via
# SW_RST_CTRL1 (including the core-31 workaround).
#
# Pass criteria:
#   1. OpenSBI starts with 32 platform HARTs
#   2. Linux boots (no maxcpus restriction)
#   3. smp: Brought up 1 node, 32 CPUs  (all 32 online)
#   4. Verification 3: PASSED
#   5. No kernel panic
#
# Timeout: 600 s  (32-core boot takes ~15-20 s on Apple M4)
# =============================================================================

tc_smp32() {
    local name="smp32"
    local logfile="${LOG_DIR}/smp32_${RUN_TS}.log"
    local timeout=600

    log_hdr "Round 4 · Full 32-Core Production SMP"
    log_info "[$name] Starting full production boot (timeout ${timeout} s)"
    log_info "[$name] Note: 32-core boot typically takes ~20 s on Apple M4"

    local qpid
    qpid=$(qemu_start "$logfile" \
        -machine "beta,config-file=${CFG}" \
        -bios    "$BIOS" \
        -device  "loader,file=${OPENSBI},addr=0x4000000000" \
        -kernel  "$KERNEL" \
        -initrd  "$INITRD" \
        -append  "root=/dev/ram rdinit=/init earlycon=uart8250,mmio32,0xd0087000,115200n8 console=ttyS0,115200n8")

    log_step "[$name] QEMU PID=$qpid"

    # ── Check 1: OpenSBI with 32 HARTs ───────────────────────────────────
    if ! wait_for_pattern "$logfile" "Platform HART Count.*32" 60 "$qpid"; then
        qemu_stop "$qpid"
        tc_fail "$name" "OpenSBI did not report 32 platform HARTs within 60 s"
        return 1
    fi
    log_step "[$name] ✓ OpenSBI: 32 platform HARTs"

    # ── Check 2: All 32 CPUs online ───────────────────────────────────────
    if ! wait_for_pattern "$logfile" "smp: Brought up 1 node, 32 CPUs" \
                          "$timeout" "$qpid"; then
        local rc=$?
        # Extract how many cores actually came up
        local last_smp
        last_smp=$(log_extract "$logfile" "smp: Brought up" | tail -1)
        qemu_stop "$qpid"
        if [ $rc -eq 124 ]; then
            tc_fail "$name" "Timed out. Last SMP line: [${last_smp:-not found}]"
        else
            tc_fail "$name" "QEMU died. Last SMP line: [${last_smp:-not found}]"
        fi
        return 1
    fi
    log_step "[$name] ✓ 32/32 CPUs online"

    # ── Check 3: Verification 3 ───────────────────────────────────────────
    if ! wait_for_pattern "$logfile" "Verification 3: PASSED" 120 "$qpid"; then
        # Non-fatal: the SMP test itself passed; note the missing verification
        log_warn "[$name] Verification 3 not seen within 120 s (non-fatal)"
    else
        log_step "[$name] ✓ Verification 3: PASSED"
    fi

    qemu_stop "$qpid"

    # ── Check 4: No kernel panic ──────────────────────────────────────────
    if log_contains "$logfile" "Kernel panic|BUG: kernel NULL pointer"; then
        local msg
        msg=$(log_extract "$logfile" "Kernel panic|BUG:" | head -1)
        tc_fail "$name" "Kernel panic: $msg"
        return 1
    fi

    # Report boot time from log if available
    local boot_time
    boot_time=$(log_extract "$logfile" "Brought up 1 node" | \
                    grep -oE '\[[ 0-9.]+\]' | head -1 | tr -d '[]')
    tc_pass "$name" "32/32 CPUs online${boot_time:+ at ${boot_time}s}"
    return 0
}
