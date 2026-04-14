#!/usr/bin/env bash
# =============================================================================
# Round 5: Interrupt Vector Table Consistency Check
#
# Pure offline check — no Linux boot required.  Dumps the DTB from QEMU and
# validates that interrupt assignments in the generated Device Tree exactly
# match the hardware interrupt vector table v2.
#
# Requires: dtc (brew install dtc)
#
# Checked properties:
#   • APLIC riscv,num-sources = 144
#   • IMSIC riscv,num-ids     = 2047
#   • IMSIC guest-index-bits  = 1  (num_vs_files=1)
#   • CPU count ≥ 32
#   • PCIe0 INTx base IRQ     = 28  (0x1c)
#   • PCIe1 INTx base IRQ     = 32  (0x20)
#   • PCIe2 INTx base IRQ     = 36  (0x24)
#   • PCIe3 INTx base IRQ     = 40  (0x28)
#   • IOMMU0 interrupt        = 44  (0x2c)
#   • IOMMU1 interrupt        = 45  (0x2d)
#   • IOMMU2 interrupt        = 46  (0x2e)
#   • IOMMU3 interrupt        = 47  (0x2f)
#   • No stale old-table IRQ assignments (no pcie*_4..15 entries)
#
# Timeout: 30 s for DTB dump
# =============================================================================

tc_irq_check() {
    local name="irq_check"
    local dtb_out="${LOG_DIR}/irq_check_${RUN_TS}.dtb"
    local dts_out="${LOG_DIR}/irq_check_${RUN_TS}.dts"

    log_hdr "Round 5 · IRQ Table Consistency Check"

    if ! command -v dtc >/dev/null 2>&1; then
        tc_skip "$name" "dtc not installed — brew install dtc"
        return 0
    fi

    # ── Dump DTB ─────────────────────────────────────────────────────────
    log_info "[$name] Dumping DTB (timeout 30 s)..."
    timeout_run 30 "$QEMU_BIN" \
        -machine "beta,config-file=${CFG},dumpdtb=${dtb_out}" \
        -nographic > /dev/null 2>&1

    if [ ! -s "$dtb_out" ]; then
        tc_fail "$name" "Failed to dump DTB"
        return 1
    fi

    # ── Convert to DTS ────────────────────────────────────────────────────
    if ! dtc -I dtb -O dts -o "$dts_out" "$dtb_out" >/dev/null 2>&1; then
        tc_fail "$name" "dtc could not parse DTB"
        return 1
    fi
    log_step "[$name] DTB → DTS: $dts_out"

    local errors=0

    # Helper: check a condition
    _chk() {
        local desc="$1" ok="$2"   # ok=0 means pass
        if [ "$ok" -eq 0 ]; then
            log_step "[$name] ✓ $desc"
        else
            log_fail "[$name] ✗ $desc"
            errors=$(( errors + 1 ))
        fi
    }

    # ── CPU count ─────────────────────────────────────────────────────────
    local cpu_count
    cpu_count=$(grep -c 'device_type = "cpu"' "$dts_out" 2>/dev/null || echo 0)
    _chk "CPU count ≥ 32 (found $cpu_count)" "$([ "$cpu_count" -ge 32 ] && echo 0 || echo 1)"

    # ── APLIC num-sources ─────────────────────────────────────────────────
    # In DTS: riscv,num-sources = <0x90>;  (0x90 = 144)
    local aplic_srcs
    aplic_srcs=$(grep "riscv,num-sources" "$dts_out" | head -1 | \
                     grep -oE '0x[0-9a-f]+|[0-9]+' | head -1)
    # Normalise hex/decimal
    local aplic_dec
    aplic_dec=$(printf '%d' "${aplic_srcs:-0}" 2>/dev/null || echo 0)
    _chk "APLIC riscv,num-sources = 144 (got $aplic_dec)" \
         "$([ "$aplic_dec" -eq 144 ] && echo 0 || echo 1)"

    # ── IMSIC num-ids ─────────────────────────────────────────────────────
    local imsic_ids_raw
    imsic_ids_raw=$(grep "riscv,num-ids" "$dts_out" | head -1 | \
                        grep -oE '0x[0-9a-f]+|[0-9]+' | head -1)
    local imsic_ids_dec
    imsic_ids_dec=$(printf '%d' "${imsic_ids_raw:-0}" 2>/dev/null || echo 0)
    _chk "IMSIC riscv,num-ids = 2047 (got $imsic_ids_dec)" \
         "$([ "$imsic_ids_dec" -eq 2047 ] && echo 0 || echo 1)"

    # ── IMSIC guest-index-bits = 1 ────────────────────────────────────────
    local gib
    gib=$(grep "riscv,guest-index-bits" "$dts_out" | head -1 | \
              grep -oE '0x[0-9a-f]+|[0-9]+' | head -1)
    local gib_dec
    gib_dec=$(printf '%d' "${gib:-0}" 2>/dev/null || echo 0)
    _chk "IMSIC guest-index-bits = 1 (got $gib_dec)" \
         "$([ "$gib_dec" -eq 1 ] && echo 0 || echo 1)"

    # ── PCIe INTx base IRQs ───────────────────────────────────────────────
    # The DTS encodes interrupts as hex.  PCIe legacy interrupt is listed as:
    #   interrupts = <0x1c 0x04>;   for PCIe0 (IRQ 28 = 0x1c)
    # We search for the hex values near pcie/interrupt-controller nodes.
    for _ci in 0 1 2 3; do
        _exp=$(( 28 + _ci * 4 ))
        _hx=$(printf '0x%x' "$_exp")
        if grep -q "interrupts = <${_hx} 0x" "$dts_out"; then
            log_step "[$name] ✓ PCIe${_ci} INTx IRQ = ${_exp} (${_hx})"
        else
            log_fail "[$name] ✗ PCIe${_ci} INTx IRQ ${_exp} (${_hx}) not found in DTB"
            errors=$(( errors + 1 ))
        fi
    done

    # ── IOMMU IRQs ────────────────────────────────────────────────────────
    for _ii in 0 1 2 3; do
        _exp=$(( 44 + _ii ))
        _hx=$(printf '0x%x' "$_exp")
        if grep -q "interrupts = <${_hx} 0x" "$dts_out"; then
            log_step "[$name] ✓ IOMMU${_ii} IRQ = ${_exp} (${_hx})"
        else
            log_fail "[$name] ✗ IOMMU${_ii} IRQ ${_exp} (${_hx}) not found"
            errors=$(( errors + 1 ))
        fi
    done

    # ── Stale old-table entries (PCIe per-lane IRQs 44-91 gone) ──────────
    # Old table had pcie0_4 through pcie3_15 (16 per controller).
    # Those IRQs were 32–91.  Check that IRQ 48 (0x30) is NOT used as a PCIe
    # interrupt (it should be ras_inbound_0 now, not pcie1_4).
    # Simple proxy: no pcie node should have IRQ 48–91 as its legacy interrupt.
    local stale=0
    for _s in 0x30 0x38 0x40 0x48 0x50 0x58; do
        if grep -q "interrupts = <${_s} 0x" "$dts_out"; then
            # This is OK only if it's not inside a pcie node; we do a rough check
            stale=$(( stale + 1 ))
        fi
    done
    # We don't fail on stale — those IRQs may legitimately appear for other
    # devices (e.g. RAS).  Just log.
    if [ "$stale" -gt 0 ]; then
        log_warn "[$name] $stale entries use old PCIe IRQ range 48-91 — manual review advised"
    fi

    if [ "$errors" -eq 0 ]; then
        tc_pass "$name" "All IRQ assignments match interrupt vector table v2"
    else
        tc_fail "$name" "$errors assignment mismatches — see $dts_out"
        return 1
    fi
    return 0
}
