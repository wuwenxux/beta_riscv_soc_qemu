#!/usr/bin/env bash
# =============================================================================
# Round 3: PCIe + VirtIO Network Data Path
#
# Builds a custom initramfs that auto-configures eth0 via DHCP and pings the
# QEMU user-mode gateway (10.0.2.2).  Boots with virtio-net-pci on the PCIe
# bus and validates the full data path.
#
# Sub-steps and individual pass/fail points:
#   A. Build net initramfs (inject /init_net + /etc/udhcpc.sh into base rootfs)
#   B. VirtIO PCI device enumerated and enabled
#   C. eth0 network interface registered (virtio_net_probe() completes)
#       └─ KNOWN GAP: currently fails here — rv-gen4-pcie MSI domain not wired
#          to PCI MSI subsystem; virtio_net_probe() doesn't complete.
#   D. DHCP lease obtained (udhcpc bound)
#   E. ping 10.0.2.2 succeeds — NETWORK_TEST_PASS printed
#
# Timeout: 180 s
# =============================================================================

tc_virtio_net() {
    local name="virtio_net"
    local logfile="${LOG_DIR}/virtio_net_${RUN_TS}.log"
    local net_initrd="${TESTS_DIR}/initramfs_net/initramfs_net.cpio.gz"
    local timeout=180

    log_hdr "Round 3 · PCIe + VirtIO Network"

    # ── Step A: Build net initramfs ───────────────────────────────────────
    log_info "[$name] Building net initramfs..."
    if ! bash "${TESTS_DIR}/initramfs_net/build_initramfs.sh" \
            --base     "$INITRD"                                      \
            --out      "$net_initrd"                                  \
            --init-src "${TESTS_DIR}/initramfs_net/init"              \
            --udhcpc   "${TESTS_DIR}/initramfs_net/etc/udhcpc.sh"; then
        tc_fail "$name" "Failed to build net initramfs (see above)"
        return 1
    fi
    log_step "[$name] Net initramfs: $net_initrd ✓"

    # ── Start QEMU ────────────────────────────────────────────────────────
    log_info "[$name] Starting QEMU with virtio-net-pci (timeout ${timeout} s)"

    local qpid
    qpid=$(qemu_start "$logfile" \
        -machine "beta,config-file=${CFG}" \
        -bios    "$BIOS" \
        -device  "loader,file=${OPENSBI},addr=0x4000000000" \
        -kernel  "$KERNEL" \
        -initrd  "$net_initrd" \
        -append  "root=/dev/ram rdinit=/init_net earlycon=uart8250,mmio32,0xd0087000,115200n8 console=ttyS0,115200n8 maxcpus=4" \
        -device  "virtio-net-pci,netdev=net0" \
        -netdev  "user,id=net0")

    log_step "[$name] QEMU PID=$qpid"

    # ── Step B: VirtIO PCI device enumerated ─────────────────────────────
    if ! wait_for_pattern "$logfile" "virtio-pci.*enabling device" 120 "$qpid"; then
        qemu_stop "$qpid"
        tc_fail "$name" "VirtIO PCI device not enumerated within 120 s"
        return 1
    fi
    log_step "[$name] ✓ VirtIO PCI device enumerated"

    # ── Step C: eth0 registration ─────────────────────────────────────────
    # Deliberate short wait: if virtio_net_probe() was going to succeed it
    # would do so within a few seconds of PCI enable.
    if ! wait_for_pattern "$logfile" "(eth0:|registered device eth0|virtio_net.*eth0)" \
                          30 "$qpid"; then
        qemu_stop "$qpid"
        # Document the known root cause
        tc_fail "$name" \
            "eth0 not registered [KNOWN GAP: rv-gen4-pcie MSI domain not propagated to PCI MSI subsystem — virtio_net_probe() incomplete]"
        log_warn "[$name] This is a known SHOULD-FIX item."
        log_warn "[$name] VirtIO PCI enumeration works; driver probe fails at MSI setup."
        return 1
    fi
    log_step "[$name] ✓ eth0 registered"

    # ── Step D: DHCP lease ────────────────────────────────────────────────
    if ! wait_for_pattern "$logfile" "udhcpc.*bound|DHCP.*lease|ip.*10\.0\.2\." \
                          30 "$qpid"; then
        qemu_stop "$qpid"
        tc_fail "$name" "eth0 registered but DHCP lease not obtained within 30 s"
        return 1
    fi
    log_step "[$name] ✓ DHCP lease obtained"

    # ── Step E: Ping gateway ──────────────────────────────────────────────
    if ! wait_for_pattern "$logfile" "NETWORK_TEST_PASS" "$timeout" "$qpid"; then
        qemu_stop "$qpid"
        if log_contains "$logfile" "NETWORK_TEST_FAIL"; then
            tc_fail "$name" "DHCP OK but ping 10.0.2.2 failed (NETWORK_TEST_FAIL)"
        else
            tc_fail "$name" "DHCP obtained but ping did not complete within timeout"
        fi
        return 1
    fi

    qemu_stop "$qpid"
    tc_pass "$name" "VirtIO eth0 ✓  DHCP ✓  ping 10.0.2.2 ✓"
    return 0
}
