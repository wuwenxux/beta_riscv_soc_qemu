#!/usr/bin/env bash
# =============================================================================
# Round 1: Smoke Test
#
# Verifies that QEMU can load the Beta SoC machine description and produce a
# valid Device Tree Blob without crashing.  No firmware or Linux kernel is
# needed.  Uses the -machine …,dumpdtb=<file> shortcut, which makes QEMU exit
# after writing the DTB.
#
# Pass criteria:
#   • QEMU exits 0 and writes a non-empty DTB within 30 s
#   • No assertion / abort message in QEMU stderr
#   • dtc can parse the DTB (if dtc is installed)
#   • DTB contains ≥ 32 CPU nodes, an APLIC node, an IMSIC node, memory nodes
#
# Timeout: 30 s
# =============================================================================

tc_smoke() {
    local name="smoke"
    local logfile="${LOG_DIR}/smoke_${RUN_TS}.log"
    local dtb_out="${LOG_DIR}/smoke_${RUN_TS}.dtb"
    local dts_out="${LOG_DIR}/smoke_${RUN_TS}.dts"

    log_hdr "Round 1 · Smoke Test"
    log_info "[$name] Dumping DTB without kernel (timeout 30 s)"

    # dumpdtb exits immediately after writing the DTB
    timeout_run 30 "$QEMU_BIN" \
        -machine "beta,config-file=${CFG},dumpdtb=${dtb_out}" \
        -nographic > "$logfile" 2>&1
    local rc=$?

    # ── Check 1: QEMU didn't hang ─────────────────────────────────────────
    if [ $rc -eq 124 ]; then
        tc_fail "$name" "QEMU hung during DTB dump (> 30 s)"
        return 1
    fi

    # ── Check 2: DTB file was created and is non-empty ────────────────────
    if [ ! -s "$dtb_out" ]; then
        log_fail "[$name] No DTB file produced. QEMU output:"
        grep -v '^$' "$logfile" | tail -10 | while IFS= read -r line; do
            log_fail "  ↳ $line"
        done
        tc_fail "$name" "No DTB produced (QEMU exit=$rc)"
        return 1
    fi
    log_step "[$name] DTB written: $(wc -c < "$dtb_out") bytes ✓"

    # ── Check 3: No assertion / abort in QEMU stderr ──────────────────────
    if grep -qE "Assertion failed:|assert.*failed|abort\(\)|QEMU: Terminated" \
            "$logfile" 2>/dev/null; then
        log_fail "[$name] QEMU assertion or abort detected:"
        grep -E "Assertion|assert|abort" "$logfile" | head -5 | \
            while IFS= read -r l; do log_fail "  ↳ $l"; done
        tc_fail "$name" "QEMU assertion/abort"
        return 1
    fi

    # ── Check 4: dtc validation (skip gracefully if dtc absent) ──────────
    if ! command -v dtc >/dev/null 2>&1; then
        log_warn "[$name] dtc not found — structural checks skipped (brew install dtc)"
        tc_pass "$name" "DTB generated, no QEMU crash (dtc skipped)"
        return 0
    fi

    if ! dtc -I dtb -O dts -o "$dts_out" "$dtb_out" >/dev/null 2>&1; then
        tc_fail "$name" "DTB failed dtc parse — corrupt or truncated"
        return 1
    fi
    log_step "[$name] dtc parse OK ✓"

    local errors=0

    # CPU count
    local cpu_count
    cpu_count=$(grep -c 'device_type = "cpu"' "$dts_out" 2>/dev/null || echo 0)
    if [ "$cpu_count" -ge 32 ]; then
        log_step "[$name] CPU nodes in DTB: $cpu_count ✓"
    else
        log_fail "[$name] Expected ≥ 32 CPU nodes, found $cpu_count"
        errors=$(( errors + 1 ))
    fi

    # APLIC node
    if grep -q "riscv,aplic" "$dts_out"; then
        log_step "[$name] APLIC node present ✓"
    else
        log_fail "[$name] APLIC node missing from DTB"
        errors=$(( errors + 1 ))
    fi

    # IMSIC node
    if grep -q "riscv,imsic" "$dts_out"; then
        log_step "[$name] IMSIC node present ✓"
    else
        log_fail "[$name] IMSIC node missing from DTB"
        errors=$(( errors + 1 ))
    fi

    # Memory nodes
    local mem_count
    mem_count=$(grep -c 'device_type = "memory"' "$dts_out" 2>/dev/null || echo 0)
    if [ "$mem_count" -ge 1 ]; then
        log_step "[$name] Memory nodes: $mem_count ✓"
    else
        log_fail "[$name] No memory nodes in DTB"
        errors=$(( errors + 1 ))
    fi

    # PCIe node
    if grep -q "rv-gen4-pcie\|designware-pcie" "$dts_out"; then
        log_step "[$name] PCIe node present ✓"
    else
        log_warn "[$name] PCIe node not found (non-fatal)"
    fi

    if [ "$errors" -eq 0 ]; then
        tc_pass "$name" "${cpu_count} CPUs, APLIC+IMSIC+mem in DTB"
    else
        tc_fail "$name" "$errors structural checks failed — see $dts_out"
        return 1
    fi
    return 0
}
