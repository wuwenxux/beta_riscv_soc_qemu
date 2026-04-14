#!/usr/bin/env bash
# =============================================================================
# Round 2: SMP Boot (4-core)
#
# Boots the full firmware + Linux stack with maxcpus=4 and verifies every
# stage of the bring-up sequence.  Each check is polled independently so the
# log shows exactly which stage failed when something goes wrong.
#
# Pass criteria (in order):
#   1. OpenSBI banner appears
#   2. IPI device is aia-imsic
#   3. Linux kernel boots (riscv: base ISA line)
#   4. smp: Brought up 1 node, 4 CPUs
#   5. APLIC reports exactly 144 interrupt sources
#   6. IMSIC reports per-CPU IDs 2047
#   7. IMSIC guest-index-bits = 1 (num_vs_files=1)
#   8. All 4 PCIe controllers init OK
#   9. initramfs Verification 3: PASSED
#
# Timeout: 120 s total (each check polls until the global deadline)
# =============================================================================

tc_smp4() {
    local name="smp4"
    local logfile="${LOG_DIR}/smp4_${RUN_TS}.log"
    local timeout=120

    log_hdr "Round 2 · SMP Boot (4-core)"
    log_info "[$name] Starting boot (timeout ${timeout} s)"

    local qpid
    qpid=$(qemu_start "$logfile" \
        -machine "beta,config-file=${CFG}" \
        -bios    "$BIOS" \
        -device  "loader,file=${OPENSBI},addr=0x4000000000" \
        -kernel  "$KERNEL" \
        -initrd  "$INITRD" \
        -append  "root=/dev/ram rdinit=/init earlycon=uart8250,mmio32,0xd0087000,115200n8 console=ttyS0,115200n8 maxcpus=4")

    log_step "[$name] QEMU PID=$qpid — waiting for boot sequence"

    # Helper: check one pattern, fail the test if not found
    _check() {
        local desc="$1" pattern="$2"
        if ! wait_for_pattern "$logfile" "$pattern" "$timeout" "$qpid"; then
            local rc=$?
            qemu_stop "$qpid"
            if [ $rc -eq 124 ]; then
                tc_fail "$name" "Timed out waiting for: $desc"
            else
                tc_fail "$name" "QEMU died before: $desc"
                log_fail "[$name] Last lines from log:"
                strings "$logfile" 2>/dev/null | tail -8 | \
                    while IFS= read -r l; do log_fail "  ↳ $l"; done
            fi
            return 1
        fi
        log_step "[$name] ✓ $desc"
        return 0
    }

    _check "OpenSBI banner"               "OpenSBI v" || return 1
    _check "IPI device = aia-imsic"       "Platform IPI Device.*aia-imsic" || return 1
    _check "Linux kernel started"         "Linux version|riscv: base ISA" || return 1
    _check "4 CPUs online"                "smp: Brought up 1 node, 4 CPUs" || return 1
    _check "APLIC 144 interrupt sources"  "riscv-aplic.*144 interrupts" || return 1
    _check "IMSIC 2047 IDs per CPU"       "per-CPU IDs 2047" || return 1
    _check "IMSIC guest-index-bits=1"     "guest-index-bits: 1" || return 1
    _check "PCIe gen4 host init OK"       "rv pcie gen4 host init ok" || return 1
    _check "initramfs Verification 3"     "Verification 3: PASSED" || return 1

    qemu_stop "$qpid"

    # Verify no panic in full log
    if log_contains "$logfile" "Kernel panic|BUG:|general protection fault"; then
        local panic
        panic=$(log_extract "$logfile" "Kernel panic|BUG:" | head -2)
        tc_fail "$name" "Kernel panic detected: $panic"
        return 1
    fi

    # Report IMSIC total interrupt count as info
    local total_irqs
    total_irqs=$(log_extract "$logfile" "total [0-9]+ interrupts available" | \
                     grep -oE '[0-9]+ interrupts' | head -1)
    tc_pass "$name" "4/4 CPUs, APLIC 144 srcs, IMSIC ${total_irqs:-2047 IDs}"
    return 0
}
