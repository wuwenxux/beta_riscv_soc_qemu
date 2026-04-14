/*
 * RISC-V cpu parameters for qemu.
 *
 * Copyright (c) 2017-2018 SiFive, Inc.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef RISCV_CPU_PARAM_H
#define RISCV_CPU_PARAM_H

#if defined(TARGET_RISCV64)
# define TARGET_PHYS_ADDR_SPACE_BITS 56 /* 44-bit PPN */
# define TARGET_VIRT_ADDR_SPACE_BITS 48 /* sv48 */
#elif defined(TARGET_RISCV32)
# define TARGET_PHYS_ADDR_SPACE_BITS 34 /* 22-bit PPN */
# define TARGET_VIRT_ADDR_SPACE_BITS 32 /* sv32 */
#endif
#define TARGET_PAGE_BITS 12 /* 4 KiB Pages */
/*
 * The current MMU Modes are:
 *  - U mode 0b000
 *  - S mode 0b001
 *  - M mode 0b011
 *  - U mode HLV/HLVX/HSV 0b100
 *  - S mode HLV/HLVX/HSV 0b101
 *  - M mode HLV/HLVX/HSV 0b111
 */

/*
 * RISC-V base memory model is RVWMO (weak ordering).
 * Set TCG_MO_ALL so that tcg_gen_req_mo() actually inserts barriers
 * when Ztso is disabled.  When Ztso is enabled, the translator adds
 * acquire/release barriers directly (via tcg_gen_mb in trans_rvi.c.inc),
 * so these req_mo barriers become redundant but harmless.
 *
 * Previously this was 0 (SC assumption), which made RVWMO barriers
 * no-ops and hid missing-fence bugs on x86 hosts.
 */
#define TCG_GUEST_DEFAULT_MO TCG_MO_ALL

#endif
