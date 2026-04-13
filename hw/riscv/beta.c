/*
 * QEMU RISC-V Beta SoC Machine
 *
 * Copyright (c) 2026
 *
 * Dual-CPU RISC-V SoC with configurable hardware via JSON config file.
 * Contains an RV32 management processor (MCPU) and RV64 application
 * processors (ACPU). All hardware parameters (addresses, sizes, interrupt
 * routing, register maps) are loaded from a JSON file at runtime.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/char/serial-mm.h"
#include "target/riscv/cpu.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/riscv/beta.h"
#include "hw/riscv/boot.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/intc/riscv_aplic.h"
#include "hw/intc/riscv_imsic.h"
#include "hw/irq.h"
#include "hw/misc/unimp.h"
#include "hw/misc/beta_generic_mmio.h"
#include "hw/misc/beta_soc_csr.h"
#include "hw/block/flash.h"
#include "hw/pci-host/designware.h"
#include "hw/pci/pci-internal.h"
#include "hw/pci/pci_host.h"
#include "hw/riscv/iommu.h"
#include "chardev/char.h"
#include "system/blockdev.h"
#include "system/device_tree.h"
#include "system/reset.h"
#include "system/system.h"
#include "exec/address-spaces.h"
#include "qemu/guest-random.h"
#include "qemu/timer.h"
#include "system/runstate.h"

#include "beta_config.h"

/* ---------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------*/

/* ceil(log2(count)) — same as virt.c's imsic_num_bits */
static uint32_t beta_num_bits(uint32_t count)
{
    uint32_t ret = 0;
    while ((1U << ret) < count) {
        ret++;
    }
    return ret;
}

/* Convert config access string to BetaRegAccessType */
static BetaRegAccessType beta_parse_access(const char *access)
{
    if (!access || g_strcmp0(access, "rw") == 0) return BETA_REG_RW;
    if (g_strcmp0(access, "ro") == 0) return BETA_REG_RO;
    if (g_strcmp0(access, "wo") == 0) return BETA_REG_WO;
    if (g_strcmp0(access, "w1c") == 0) return BETA_REG_W1C;
    if (g_strcmp0(access, "rc") == 0) return BETA_REG_RC;
    return BETA_REG_RW;
}

/*
 * Convert BetaRegDef array (config) to BetaMMIORegDef array (device).
 * Names are borrowed (not owned) from the config — the config must outlive
 * the device.  Caller must g_free() the returned array.
 */
static BetaMMIORegDef *beta_convert_reg_defs(BetaRegDef *src,
                                              uint32_t num_regs)
{
    uint32_t i;
    BetaMMIORegDef *dst;

    if (!src || num_regs == 0) {
        return NULL;
    }
    dst = g_new0(BetaMMIORegDef, num_regs);
    for (i = 0; i < num_regs; i++) {
        dst[i].name        = src[i].name;   /* borrowed pointer */
        dst[i].offset      = src[i].offset;
        dst[i].width       = src[i].width ? src[i].width : 4;
        dst[i].reset_value = src[i].reset_value;
        dst[i].access      = beta_parse_access(src[i].access);
    }
    return dst;
}

/* Find IRQ number for a named source in the irq_map */
static int beta_find_irq(BetaSoCConfig *cfg, const char *name)
{
    uint32_t i;
    for (i = 0; i < cfg->interrupts.num_irqs; i++) {
        if (g_strcmp0(cfg->interrupts.irq_map[i].name, name) == 0) {
            return (int)cfg->interrupts.irq_map[i].irq_num;
        }
    }
    return 0;
}

/* Find a device config entry by type (first match) */
static BetaDeviceConfig *beta_find_device_by_type(BetaSoCConfig *cfg,
                                                   const char *type)
{
    uint32_t i;
    for (i = 0; i < cfg->num_devices; i++) {
        if (g_strcmp0(cfg->devices[i].type, type) == 0) {
            return &cfg->devices[i];
        }
    }
    return NULL;
}

/*
 * Target-specific callback for setting ACPU a1 (boot argument / FDT address).
 * Called from the SoC CSR CORE_RELEASE handler after cpu_reset().
 * Must live in beta.c because it accesses RISCVCPU internals.
 */
static void beta_set_acpu_a0(CPUState *cpu, uint64_t hartid)
{
    RISCVCPU *riscv_cpu = RISCV_CPU(cpu);
    riscv_cpu->env.gpr[10] = hartid;  /* a0 = hartid */
}

static void beta_set_acpu_a1(CPUState *cpu, uint64_t arg)
{
    RISCVCPU *riscv_cpu = RISCV_CPU(cpu);
    riscv_cpu->env.gpr[11] = arg;  /* a1 = FDT/boot arg */
}

/* ---------------------------------------------------------------------------
 * CPU type selection
 * -------------------------------------------------------------------------*/

static const char *beta_cpu_type_from_isa(const char *isa)
{
    if (g_str_has_prefix(isa, "rv32")) {
        return TYPE_RISCV_CPU_BASE32;
    } else if (g_str_has_prefix(isa, "rv64")) {
        /*
         * Use RVA23S64 profile: enables V, H, and all RVA23 mandatory
         * extensions at the CPU type level (before realize).
         * VLEN/ELEN defaults from profile: VLEN=256, ELEN=64.
         * Beta needs VLEN=512 — set in beta_create_acpu after realize.
         */
        return TYPE_RISCV_CPU_RVA23S64;
    }
    return TYPE_RISCV_CPU_BASE;
}

static inline uint32_t acpu_total_harts(BetaSoCConfig *cfg)
{
    return cfg->acpu.num_clusters * cfg->acpu.cores_per_cluster;
}

/* ---------------------------------------------------------------------------
 * Step 2: MCPU hart array
 * -------------------------------------------------------------------------*/

static void beta_create_mcpu(BetaMachineState *s,
                             MemoryRegion *system_memory)
{
    BetaMCPUConfig *mcpu = &s->config->mcpu;
    const char *cpu_type = beta_cpu_type_from_isa(mcpu->isa);

    object_initialize_child(OBJECT(s), "mcpu-soc", &s->mcpu_soc,
                            TYPE_RISCV_HART_ARRAY);
    object_property_set_str(OBJECT(&s->mcpu_soc), "cpu-type",
                            cpu_type, &error_abort);
    /*
     * MCPU hartid = 32 (after all 32 ACPU cores, hartid 0-31).
     * DTS: cpu0-cpu31 are ACPU with reg=0..31, MCPU is hartid 32.
     */
    object_property_set_int(OBJECT(&s->mcpu_soc), "hartid-base",
                            acpu_total_harts(s->config), &error_abort);
    object_property_set_int(OBJECT(&s->mcpu_soc), "num-harts",
                            mcpu->num_cores, &error_abort);
    object_property_set_int(OBJECT(&s->mcpu_soc), "resetvec",
                            mcpu->reset_vector, &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&s->mcpu_soc), &error_fatal);
}

/* ---------------------------------------------------------------------------
 * Step 3: ACPU hart array (all start powered-off)
 * -------------------------------------------------------------------------*/

/*
 * Pre-realize hook for each ACPU hart (BUG-P0-01/02/04).
 * Called by riscv_hart_realize() after object_initialize_child() but
 * before qdev_realize(), so that the CPU's realize path sees all
 * properties in their final state.
 */
static void beta_acpu_pre_realize(RISCVCPU *cpu, int idx G_GNUC_UNUSED,
                                  void *opaque G_GNUC_UNUSED)
{
    CPURISCVState *env = &cpu->env;

    /* BUG-P0-01: enable AIA supervisor extensions so siselect/sireg/stopei
     * CSRs are accessible; DTB already declares smaia/ssaia. */
    object_property_set_bool(OBJECT(cpu), "smaia", true, &error_abort);
    object_property_set_bool(OBJECT(cpu), "ssaia", true, &error_abort);

    /* BUG-P0-02: H-extension (Hypervisor) is not part of the RVA23S64
     * profile; set misa_ext before realize so the CPU's realize path
     * allocates hypervisor CSR state and the bits survive cpu_reset(). */
    env->misa_ext      |= RVH;
    env->misa_ext_mask |= RVH;

    /* BUG-P0-04: VLEN=512.  The RVA23S64 profile defaults to VLEN=128;
     * vector register storage is allocated during realize, so this MUST
     * be set before qdev_realize(). */
    object_property_set_uint(OBJECT(cpu), "vlen", 512, &error_abort);

    /* Scalar crypto: only Zkt (timing-safe attribute) is supported per ISA Status.
     * Zkn/Zks instruction-set extensions are NOT implemented in Beta hardware. */
}

static void beta_create_acpu(BetaMachineState *s,
                             MemoryRegion *system_memory)
{
    BetaACPUConfig *acpu = &s->config->acpu;
    const char *cpu_type = beta_cpu_type_from_isa(acpu->isa);
    uint32_t num_acpu_harts = acpu->num_clusters * acpu->cores_per_cluster;
    int i;

    object_initialize_child(OBJECT(s), "acpu-soc", &s->acpu_soc,
                            TYPE_RISCV_HART_ARRAY);
    object_property_set_str(OBJECT(&s->acpu_soc), "cpu-type",
                            cpu_type, &error_abort);
    /* ACPU hartid base from config (default 0: cpu0 reg=0 .. cpuN reg=N) */
    object_property_set_int(OBJECT(&s->acpu_soc), "hartid-base",
                            acpu->hartid_base, &error_abort);
    object_property_set_int(OBJECT(&s->acpu_soc), "num-harts",
                            num_acpu_harts, &error_abort);
    object_property_set_int(OBJECT(&s->acpu_soc), "resetvec",
                            acpu->reset_vector, &error_abort);

    /* Register pre-realize hook so smaia/ssaia/H-ext/vlen are set
     * before each individual hart's qdev_realize() runs. */
    s->acpu_soc.pre_realize_fn     = beta_acpu_pre_realize;
    s->acpu_soc.pre_realize_opaque = s;

    sysbus_realize(SYS_BUS_DEVICE(&s->acpu_soc), &error_fatal);

    /* Post-realize: set runtime cfg flags and machine-identification CSRs.
     * These fields are read at runtime (not used by realize-time allocation),
     * so post-realize assignment is correct. */
    for (i = 0; i < (int)num_acpu_harts; i++) {
        RISCVCPU *cpu = &s->acpu_soc.harts[i];

        cpu->cfg.ext_ztso = s->ztso;  /* TSO or RVWMO (-M beta,ztso=on/off) */

        /* BUG-P0-03: machine-identification CSRs.  The rva23s64 profile CPU
         * type is not a "dynamic" CPU so the property setter rejects changes;
         * write the cfg struct directly (same as csr.c read_mvendorid does). */
        cpu->cfg.mvendorid = acpu->mvendorid;
        cpu->cfg.marchid   = acpu->marchid;
        cpu->cfg.mimpid    = acpu->mimpid;
    }
    if (!s->ztso) {
        qemu_log_mask(LOG_UNIMP,
                      "beta: Ztso DISABLED — running in RVWMO mode "
                      "(fence bugs will be exposed)\n");
    }

    /*
     * In mcpu_bootrom mode, ACPU cores start powered-off and are
     * released by MCPU via CORE_RELEASE.
     * In acpu_direct mode, ACPU cores start running immediately.
     */
    if (g_strcmp0(s->config->boot.mode, "acpu_direct") != 0) {
        for (i = 0; i < (int)num_acpu_harts; i++) {
            CPUState *cs = CPU(&s->acpu_soc.harts[i]);
            object_property_set_bool(OBJECT(cs), "start-powered-off", true,
                                     &error_abort);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Step 4: Memory regions (RAM, ROM) from config
 * -------------------------------------------------------------------------*/

static void beta_create_memory_regions(BetaMachineState *s,
                                       MemoryRegion *system_memory)
{
    BetaSoCConfig *cfg = s->config;
    uint32_t i;

    /*
     * Create per-channel DDR RAM regions.
     *
     * Beta hardware has 8 DDR channels at non-contiguous physical addresses
     * (e.g. 0x4000000000, 0x5000000000, ..., 0xB000000000). Each channel is
     * independent; the DTB exports them as separate /memory nodes with NUMA
     * tags. QEMU creates one MemoryRegion per channel so that the physical
     * addresses advertised in the DTB are actually backed by RAM.
     *
     * The FDT and kernel are placed in channel 0 (see beta_machine_done).
     */
    for (i = 0; i < cfg->ddr.num_channels; i++) {
        hwaddr ch_base = cfg->ddr.channels[i].mem_base;
        hwaddr ch_size = cfg->ddr.channels[i].size;
        if (ch_base == 0 || ch_size == 0) {
            continue;
        }
        MemoryRegion *ch_ram = g_new(MemoryRegion, 1);
        g_autofree char *ch_name = g_strdup_printf("beta.ddr-ch%u", i);
        memory_region_init_ram(ch_ram, NULL, ch_name, ch_size, &error_fatal);
        memory_region_add_subregion(system_memory, ch_base, ch_ram);
    }

    /* Create non-DDR memory regions (ROM, SRAM, L3 OCM, etc.) */
    for (i = 0; i < cfg->num_memory_regions; i++) {
        BetaMemRegion *mr = &cfg->memory_regions[i];
        MemoryRegion *region;

        /* Skip DDR regions — handled above as interleaved block */
        if (g_str_has_prefix(mr->name, "ddr")) {
            continue;
        }

        region = g_new(MemoryRegion, 1);
        if (g_strcmp0(mr->type, "ram") == 0) {
            memory_region_init_ram(region, NULL, mr->name,
                                   mr->size, &error_fatal);
            memory_region_add_subregion(system_memory, mr->base, region);
        } else if (g_strcmp0(mr->type, "rom") == 0) {
            memory_region_init_rom(region, NULL, mr->name,
                                   mr->size, &error_fatal);
            memory_region_add_subregion(system_memory, mr->base, region);
        } else {
            g_free(region);
        }
    }

    /* MCPU ITCM and DTCM (tightly-coupled memories) */
    {
        BetaMCPUConfig *mcpu = &cfg->mcpu;
        if (mcpu->itcm_kb > 0) {
            MemoryRegion *itcm = g_new(MemoryRegion, 1);
            memory_region_init_ram(itcm, NULL, "beta.mcpu-itcm",
                                  mcpu->itcm_kb * KiB, &error_fatal);
            memory_region_add_subregion(system_memory, 0xE4600000, itcm);
        }
        if (mcpu->dtcm_kb > 0) {
            MemoryRegion *dtcm = g_new(MemoryRegion, 1);
            memory_region_init_ram(dtcm, NULL, "beta.mcpu-dtcm",
                                  mcpu->dtcm_kb * KiB, &error_fatal);
            memory_region_add_subregion(system_memory, 0xE4800000, dtcm);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Step 5: Load -bios firmware into boot ROM region
 * -------------------------------------------------------------------------*/

static void beta_load_firmware(BetaMachineState *s)
{
    MachineState *machine = MACHINE(s);
    BetaBootConfig *boot = &s->config->boot;

    if (machine->firmware) {
        hwaddr addr = boot->bootrom_base;
        riscv_load_firmware(machine->firmware, &addr, NULL);
    }
}

/* ---------------------------------------------------------------------------
 * Step 7: ACPU CLINT (ACLINT MSWI + MTIMER)
 * -------------------------------------------------------------------------*/

static void beta_create_clint(BetaMachineState *s)
{
    BetaCLINTConfig *ccfg = &s->config->interrupts.clint;
    uint32_t num_acpu = s->config->acpu.num_clusters *
                        s->config->acpu.cores_per_cluster;
    uint32_t acpu_base_hartid = s->config->acpu.hartid_base;
    uint32_t freq;

    if (ccfg->base == 0) {
        return;  /* CLINT not configured */
    }

    freq = ccfg->timebase_freq ? ccfg->timebase_freq
                               : RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ;

    /* MSWI (machine-mode software interrupts) */
    riscv_aclint_swi_create(ccfg->base,
                            acpu_base_hartid, num_acpu,
                            false /* not SSWI */);

    /* MTIMER (machine-mode timer) */
    riscv_aclint_mtimer_create(
        ccfg->base + RISCV_ACLINT_SWI_SIZE,
        RISCV_ACLINT_DEFAULT_MTIMER_SIZE,
        acpu_base_hartid, num_acpu,
        RISCV_ACLINT_DEFAULT_MTIMECMP,
        RISCV_ACLINT_DEFAULT_MTIME,
        freq, true /* provide rdtime */);

    /*
     * MCPU Platform Timer @ 0xE004_1000
     * Same shared CLINT, but MCPU (RV32, hartid=32) accesses it via
     * Subsystem IO space. Standard CLINT layout; 64-bit registers
     * (mtime, mtimecmp) accessed as high/low 32-bit pairs on RV32.
     */
    {
        uint32_t mcpu_hartid = num_acpu;  /* hartid 32 */
        hwaddr mcpu_timer_base = 0xE0041000;

        riscv_aclint_swi_create(mcpu_timer_base,
                                mcpu_hartid, 1,
                                false /* not SSWI */);

        riscv_aclint_mtimer_create(
            mcpu_timer_base + RISCV_ACLINT_SWI_SIZE,
            RISCV_ACLINT_DEFAULT_MTIMER_SIZE,
            mcpu_hartid, 1,
            RISCV_ACLINT_DEFAULT_MTIMECMP,
            RISCV_ACLINT_DEFAULT_MTIME,
            freq, true /* provide rdtime */);
    }
}

/* ---------------------------------------------------------------------------
 * Step 7b: MCPU subsystem PLIC @ 0xE004_0000
 *
 * Custom compact PLIC (NOT SiFive layout) for MCPU subsystem-internal
 * interrupts (Debug Module, Error Syndrome, etc., 64 sources).
 * This is separate from MLROOT APLIC which handles SoC-level interrupts.
 *
 * Register layout (from MCPU_R1_Memory_Map_Register_List.xlsx):
 *   0x00-0x04  EL (edge/level, 64 sources: 0=level, 1=edge)
 *   0x08-0x0c  SM (source mask: 1=masked)
 *   0x10-0x14  DBG_EN (debug enable)
 *   0x18-0x34  Priority (8 x 32-bit regs, 8-bit priority per source)
 *   0x38-0x3c  IE (interrupt enable: 1=enabled)
 *   0x40       Threshold (priority threshold)
 *   0x44       ID (claim/complete: read=highest pending ID, write=complete)
 * -------------------------------------------------------------------------*/

#define MCPU_PLIC_NUM_SOURCES   64

/* Register offsets */
#define MCPU_PLIC_EL_LO         0x00
#define MCPU_PLIC_EL_HI         0x04
#define MCPU_PLIC_SM_LO         0x08
#define MCPU_PLIC_SM_HI         0x0C
#define MCPU_PLIC_DBG_EN_LO     0x10
#define MCPU_PLIC_DBG_EN_HI     0x14
#define MCPU_PLIC_PRIORITY_BASE 0x18  /* 8 x 32-bit regs (0x18..0x34) */
#define MCPU_PLIC_IE_LO         0x38
#define MCPU_PLIC_IE_HI         0x3C
#define MCPU_PLIC_THRESHOLD     0x40
#define MCPU_PLIC_ID            0x44

typedef struct BetaMcpuPLICState {
    MemoryRegion mr;
    uint64_t edge_level;     /* EL: 0=level-triggered, 1=edge-triggered */
    uint64_t source_mask;    /* SM: 1=masked (blocked) */
    uint64_t debug_en;       /* DBG_EN */
    uint8_t  priority[MCPU_PLIC_NUM_SOURCES]; /* per-source priority */
    uint64_t ie;             /* IE: 1=enabled */
    uint32_t threshold;      /* priority threshold */
    uint64_t ip;             /* IP: 1=pending (set by HW, cleared by claim) */
    qemu_irq mcpu_ext_irq;  /* output: drives MCPU IRQ_M_EXT */
} BetaMcpuPLICState;

/*
 * Compute the highest-priority pending+enabled+unmasked interrupt ID.
 * Returns 0 if no interrupt qualifies (ID 0 means "no interrupt").
 * Source numbering: 1..63 (source 0 is reserved/unused, matching PLIC
 * convention).
 */
static uint32_t beta_mcpu_plic_best_id(BetaMcpuPLICState *s)
{
    uint64_t active = s->ip & s->ie & ~s->source_mask;
    uint32_t best_id = 0;
    uint8_t  best_prio = 0;
    int i;

    while (active) {
        i = ctz64(active);          /* lowest set bit */
        active &= active - 1;      /* clear it */
        if (s->priority[i] > s->threshold && s->priority[i] > best_prio) {
            best_prio = s->priority[i];
            best_id = i;
        }
    }
    return best_id;
}

/* Re-evaluate whether to assert the external interrupt to MCPU */
static void beta_mcpu_plic_update(BetaMcpuPLICState *s)
{
    uint64_t active = s->ip & s->ie & ~s->source_mask;
    bool raise = false;
    int i;

    /* Check if any active source exceeds the threshold */
    while (active) {
        i = ctz64(active);
        active &= active - 1;
        if (s->priority[i] > s->threshold) {
            raise = true;
            break;
        }
    }
    qemu_set_irq(s->mcpu_ext_irq, raise ? 1 : 0);
}

/* Input IRQ handler: called when a subsystem device asserts/deasserts */
static void beta_mcpu_plic_set_irq(void *opaque, int irq, int level)
{
    BetaMcpuPLICState *s = opaque;

    if (irq < 0 || irq >= MCPU_PLIC_NUM_SOURCES) {
        return;
    }

    if (level) {
        s->ip |= (1ULL << irq);
    } else if (!(s->edge_level & (1ULL << irq))) {
        /* Level-triggered: clear pending when source deasserts */
        s->ip &= ~(1ULL << irq);
    }
    /* Edge-triggered pending bits stay set until software claims them */

    beta_mcpu_plic_update(s);
}

static uint64_t beta_mcpu_plic_read(void *opaque, hwaddr addr, unsigned size)
{
    BetaMcpuPLICState *s = opaque;

    switch (addr) {
    case MCPU_PLIC_EL_LO:
        return (uint32_t)s->edge_level;
    case MCPU_PLIC_EL_HI:
        return (uint32_t)(s->edge_level >> 32);
    case MCPU_PLIC_SM_LO:
        return (uint32_t)s->source_mask;
    case MCPU_PLIC_SM_HI:
        return (uint32_t)(s->source_mask >> 32);
    case MCPU_PLIC_DBG_EN_LO:
        return (uint32_t)s->debug_en;
    case MCPU_PLIC_DBG_EN_HI:
        return (uint32_t)(s->debug_en >> 32);
    case MCPU_PLIC_IE_LO:
        return (uint32_t)s->ie;
    case MCPU_PLIC_IE_HI:
        return (uint32_t)(s->ie >> 32);
    case MCPU_PLIC_THRESHOLD:
        return s->threshold;
    case MCPU_PLIC_ID:
        /* Read returns highest-priority pending interrupt ID (0 = none) */
        return beta_mcpu_plic_best_id(s);
    default:
        break;
    }

    /* Priority registers: 0x18..0x34 (8 x 32-bit, each packs 4 x 8-bit) */
    if (addr >= MCPU_PLIC_PRIORITY_BASE && addr <= 0x34 && (addr & 3) == 0) {
        uint32_t idx = (addr - MCPU_PLIC_PRIORITY_BASE) >> 2;
        uint32_t base_src = idx * 4;
        return (uint32_t)s->priority[base_src]
             | ((uint32_t)s->priority[base_src + 1] << 8)
             | ((uint32_t)s->priority[base_src + 2] << 16)
             | ((uint32_t)s->priority[base_src + 3] << 24);
    }

    return 0;
}

static void beta_mcpu_plic_write(void *opaque, hwaddr addr,
                                 uint64_t val, unsigned size)
{
    BetaMcpuPLICState *s = opaque;
    uint32_t v = (uint32_t)val;

    switch (addr) {
    case MCPU_PLIC_EL_LO:
        s->edge_level = (s->edge_level & 0xFFFFFFFF00000000ULL) | v;
        return;
    case MCPU_PLIC_EL_HI:
        s->edge_level = (s->edge_level & 0x00000000FFFFFFFFULL)
                      | ((uint64_t)v << 32);
        return;
    case MCPU_PLIC_SM_LO:
        s->source_mask = (s->source_mask & 0xFFFFFFFF00000000ULL) | v;
        beta_mcpu_plic_update(s);
        return;
    case MCPU_PLIC_SM_HI:
        s->source_mask = (s->source_mask & 0x00000000FFFFFFFFULL)
                       | ((uint64_t)v << 32);
        beta_mcpu_plic_update(s);
        return;
    case MCPU_PLIC_DBG_EN_LO:
        s->debug_en = (s->debug_en & 0xFFFFFFFF00000000ULL) | v;
        return;
    case MCPU_PLIC_DBG_EN_HI:
        s->debug_en = (s->debug_en & 0x00000000FFFFFFFFULL)
                    | ((uint64_t)v << 32);
        return;
    case MCPU_PLIC_IE_LO:
        s->ie = (s->ie & 0xFFFFFFFF00000000ULL) | v;
        beta_mcpu_plic_update(s);
        return;
    case MCPU_PLIC_IE_HI:
        s->ie = (s->ie & 0x00000000FFFFFFFFULL) | ((uint64_t)v << 32);
        beta_mcpu_plic_update(s);
        return;
    case MCPU_PLIC_THRESHOLD:
        s->threshold = v;
        beta_mcpu_plic_update(s);
        return;
    case MCPU_PLIC_ID:
        /*
         * Write to ID = claim/complete: clear the pending bit for the
         * claimed interrupt source so it can re-trigger.
         */
        if (v > 0 && v < MCPU_PLIC_NUM_SOURCES) {
            s->ip &= ~(1ULL << v);
            beta_mcpu_plic_update(s);
        }
        return;
    default:
        break;
    }

    /* Priority registers: 0x18..0x34 */
    if (addr >= MCPU_PLIC_PRIORITY_BASE && addr <= 0x34 && (addr & 3) == 0) {
        uint32_t idx = (addr - MCPU_PLIC_PRIORITY_BASE) >> 2;
        uint32_t base_src = idx * 4;
        s->priority[base_src]     = (uint8_t)(v);
        s->priority[base_src + 1] = (uint8_t)(v >> 8);
        s->priority[base_src + 2] = (uint8_t)(v >> 16);
        s->priority[base_src + 3] = (uint8_t)(v >> 24);
        beta_mcpu_plic_update(s);
        return;
    }
}

static const MemoryRegionOps beta_mcpu_plic_ops = {
    .read = beta_mcpu_plic_read,
    .write = beta_mcpu_plic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void beta_create_mcpu_plic(BetaMachineState *s,
                                  MemoryRegion *system_memory)
{
    BetaMcpuPLICState *ps = g_new0(BetaMcpuPLICState, 1);
    uint32_t mcpu_hartid = acpu_total_harts(s->config);

    /*
     * Create 64 input IRQ lines. Subsystem devices call
     * qemu_set_irq(irqs[n], level) to assert/deassert source n.
     */
    s->mcpu_plic_irqs = qemu_allocate_irqs(beta_mcpu_plic_set_irq, ps,
                                            MCPU_PLIC_NUM_SOURCES);

    /*
     * Connect the PLIC output to MCPU core's M-mode external interrupt.
     * MCPU hartid = acpu_total_harts (typically 32). The RISCV CPU device
     * exposes GPIO inputs indexed by interrupt cause number.
     */
    CPUState *cpu = qemu_get_cpu(mcpu_hartid);
    g_assert(cpu != NULL);
    ps->mcpu_ext_irq = qdev_get_gpio_in(DEVICE(cpu), IRQ_M_EXT);

    memory_region_init_io(&ps->mr, NULL, &beta_mcpu_plic_ops,
                          ps, "beta.mcpu-plic", 0x1000);
    memory_region_add_subregion(system_memory, 0xE0040000, &ps->mr);
    s->mcpu_plic = ps;
}

/* ---------------------------------------------------------------------------
 * Step 7c: MCPU AHB master window — 40-bit address translation
 *
 * MCPU is RV32 (32-bit address space) but the SoC has a 40-bit physical
 * address space.  The mcpu_ext_addr register (Platform Timer extended CSR
 * at 0xE004_D00C) provides AHB master address bits [39:32].  When MCPU
 * accesses the AHB master window (0xA000_0000 – 0xAFFF_FFFF), the
 * effective physical address is:
 *
 *   PA[39:0] = { ext_addr[7:0], offset[27:0] }
 *
 * This gives MCPU access to the full SoC address space (1 TiB).
 * -------------------------------------------------------------------------*/

#define BETA_MCPU_AHB_WINDOW_BASE   0xA0000000ULL
#define BETA_MCPU_AHB_WINDOW_SIZE   (256 * MiB)    /* 0xA000_0000 – 0xAFFF_FFFF */
#define BETA_MCPU_EXT_ADDR_BASE     0xE004D000ULL
#define BETA_MCPU_EXT_ADDR_SIZE     0x1000          /* 4KB register block */
#define BETA_MCPU_EXT_ADDR_REG_OFF  0x00C           /* offset within block */

typedef struct BetaMcpuAHBState {
    MemoryRegion ahb_mr;        /* AHB master window (256MB) */
    MemoryRegion ext_addr_mr;   /* ext_addr register block (4KB) */
    AddressSpace *target_as;    /* system (ACPU) address space */
    uint8_t ext_addr;           /* upper 8 address bits [39:32] */
    /*
     * Minimum legal translated ACPU physical address for AHB window accesses.
     * ext_addr == 0x00 maps to the MCPU-local 4 GB range
     * (PA 0x0_0000_0000–0x0_FFFF_FFFF).  Routing a window access there
     * bypasses the intended NOC path to ACPU memory and is almost certainly
     * a firmware misconfiguration.  Accesses whose translated PA falls below
     * this threshold are rejected with LOG_GUEST_ERROR.
     */
    hwaddr acpu_addr_base;
} BetaMcpuAHBState;

/* --- AHB master window read/write --- */

static uint64_t beta_mcpu_ahb_read(void *opaque, hwaddr offset, unsigned size)
{
    BetaMcpuAHBState *ahb = opaque;
    MemTxResult res;
    uint64_t val = 0;
    hwaddr full_addr;

    /* Sanity: offset must lie within the 256 MB window (28-bit field). */
    if (offset >= BETA_MCPU_AHB_WINDOW_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "beta-mcpu-ahb: read offset 0x%" HWADDR_PRIx
                      " out of window bounds (max 0x%x)\n",
                      offset, (unsigned)BETA_MCPU_AHB_WINDOW_SIZE - 1);
        return ~0ULL;
    }

    full_addr = ((hwaddr)ahb->ext_addr << 32) | offset;

    if (full_addr < ahb->acpu_addr_base) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "beta: AHB window read at illegal address 0x%" PRIx64
                      " (ext_addr=0x%02x routes to MCPU-local space;"
                      " expected ext_addr >= 0x01)\n",
                      (uint64_t)full_addr, ahb->ext_addr);
        return 0xDEADBEEFULL;
    }

    res = address_space_read(ahb->target_as, full_addr,
                             MEMTXATTRS_UNSPECIFIED, &val, size);
    if (res != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "beta-mcpu-ahb: read failed at PA 0x%" HWADDR_PRIx
                      " (ext_addr=0x%02x window_off=0x%" HWADDR_PRIx
                      " size=%u) MemTxResult=%d\n",
                      full_addr, ahb->ext_addr, offset, size, (int)res);
        return ~0ULL;
    }
    return val;
}

static void beta_mcpu_ahb_write(void *opaque, hwaddr offset,
                                uint64_t val, unsigned size)
{
    BetaMcpuAHBState *ahb = opaque;
    MemTxResult res;
    hwaddr full_addr;

    /* Sanity: offset must lie within the 256 MB window (28-bit field). */
    if (offset >= BETA_MCPU_AHB_WINDOW_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "beta-mcpu-ahb: write offset 0x%" HWADDR_PRIx
                      " out of window bounds (max 0x%x)"
                      " val=0x%" PRIx64 " size=%u\n",
                      offset, (unsigned)BETA_MCPU_AHB_WINDOW_SIZE - 1,
                      val, size);
        return;
    }

    full_addr = ((hwaddr)ahb->ext_addr << 32) | offset;

    if (full_addr < ahb->acpu_addr_base) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "beta: AHB window write at illegal address 0x%" PRIx64
                      " (ext_addr=0x%02x routes to MCPU-local space;"
                      " expected ext_addr >= 0x01) val=0x%" PRIx64 "\n",
                      (uint64_t)full_addr, ahb->ext_addr, val);
        return;
    }

    res = address_space_write(ahb->target_as, full_addr,
                              MEMTXATTRS_UNSPECIFIED, &val, size);
    if (res != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "beta-mcpu-ahb: write failed at PA 0x%" HWADDR_PRIx
                      " (ext_addr=0x%02x window_off=0x%" HWADDR_PRIx
                      " size=%u val=0x%" PRIx64 ") MemTxResult=%d\n",
                      full_addr, ahb->ext_addr, offset, size, val, (int)res);
    }
}

static const MemoryRegionOps beta_mcpu_ahb_ops = {
    .read = beta_mcpu_ahb_read,
    .write = beta_mcpu_ahb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/* --- ext_addr register (Platform Timer extended CSR) --- */

static uint64_t beta_mcpu_ext_addr_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    BetaMcpuAHBState *ahb = opaque;

    if (offset == BETA_MCPU_EXT_ADDR_REG_OFF) {
        return ahb->ext_addr;
    }
    return 0;  /* other offsets in this block read as zero */
}

static void beta_mcpu_ext_addr_write(void *opaque, hwaddr offset,
                                     uint64_t val, unsigned size)
{
    BetaMcpuAHBState *ahb = opaque;

    if (offset == BETA_MCPU_EXT_ADDR_REG_OFF) {
        ahb->ext_addr = (uint8_t)(val & 0xFF);
    }
    /* writes to other offsets are ignored */
}

static const MemoryRegionOps beta_mcpu_ext_addr_ops = {
    .read = beta_mcpu_ext_addr_read,
    .write = beta_mcpu_ext_addr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/* --- Init function --- */

static void beta_create_mcpu_ahb_window(BetaMachineState *s,
                                        MemoryRegion *system_memory)
{
    BetaMcpuAHBState *ahb = g_new0(BetaMcpuAHBState, 1);

    ahb->target_as = &address_space_memory;
    ahb->ext_addr = 0;  /* reset: no upper address bits */
    /*
     * Reject translated addresses below 4 GB (ext_addr == 0x00).
     * The AHB window is meant to access ACPU's high address space
     * (CSR @ 0x1_0000_0000+, DDR @ 0x40_0000_0000+).  ext_addr == 0
     * maps to MCPU's own local 4 GB — that path is almost certainly a bug.
     */
    ahb->acpu_addr_base = 0x100000000ULL;

    /* AHB master window: 256MB at 0xA000_0000 in MCPU address space */
    memory_region_init_io(&ahb->ahb_mr, NULL, &beta_mcpu_ahb_ops,
                          ahb, "beta.mcpu-ahb-window",
                          BETA_MCPU_AHB_WINDOW_SIZE);
    memory_region_add_subregion(system_memory,
                                BETA_MCPU_AHB_WINDOW_BASE, &ahb->ahb_mr);

    /* ext_addr register: 4KB block at 0xE004_D000 */
    memory_region_init_io(&ahb->ext_addr_mr, NULL, &beta_mcpu_ext_addr_ops,
                          ahb, "beta.mcpu-ext-addr",
                          BETA_MCPU_EXT_ADDR_SIZE);
    memory_region_add_subregion(system_memory,
                                BETA_MCPU_EXT_ADDR_BASE, &ahb->ext_addr_mr);

    s->mcpu_ahb = ahb;
}

/* ---------------------------------------------------------------------------
 * Step 8: ACPU AIA (APLIC M/S + IMSIC M/S per-hart)
 * -------------------------------------------------------------------------*/

static void beta_create_aia(BetaMachineState *s)
{
    BetaAPLICConfig *acfg = &s->config->interrupts.aplic;
    BetaIMSICConfig *icfg = &s->config->interrupts.imsic;
    uint32_t num_acpu = acpu_total_harts(s->config);
    uint32_t cores_per_cluster = s->config->acpu.cores_per_cluster;
    uint32_t num_mcpu = s->config->mcpu.num_cores;
    /*
     * MCPU hartid must match what beta_create_mcpu() actually set:
     * hartid-base = acpu_total_harts (not the config field, which is fixed
     * at 32 for the 8-cluster production config but wrong for other cluster
     * counts).  The config field is kept for documentation only.
     */
    uint32_t mcpu_hartid_base = num_acpu;
    uint32_t num_sources = acfg->num_sources ? acfg->num_sources : 1023;
    BetaAPLICDomain *root_domain = NULL, *m_domain = NULL, *s_domain = NULL;
    uint32_t guest_bits;
    uint32_t i;

    /*
     * Per-cluster IMSIC group stride: each cluster's CSR space is 16MB apart.
     * group_index_shift = 24 (2^24 = 0x1000000).
     */
    const hwaddr group_stride = (hwaddr)1 << IMSIC_MMIO_GROUP_MIN_SHIFT;

    if (icfg->m_base == 0 || icfg->s_base == 0) {
        return;  /* IMSIC not configured, skip AIA */
    }

    guest_bits = (icfg->num_vs_files > 0) ?
                 beta_num_bits(icfg->num_vs_files + 1) : 0;

    /*
     * Per-hart M-level IMSIC (ACPU only, MCPU has no IMSIC).
     * IMSICs are placed inside each cluster's CSR address space:
     *   addr = m_base + cluster * group_stride + core_in_cluster * hart_size
     * where m_base is the M-IMSIC base of cluster 0 (0x0710200000).
     *
     * Store device pointers so beta_create_soc_csr() can pass them to the CSR
     * device for SMP bringup eidelivery pre-enable (see beta_soc_csr.c).
     */
    s->acpu_m_imsic = g_new0(DeviceState *, num_acpu);
    s->num_acpu_m_imsic = num_acpu;
    for (i = 0; i < num_acpu; i++) {
        uint32_t cluster = i / cores_per_cluster;
        uint32_t core_in_cluster = i % cores_per_cluster;
        s->acpu_m_imsic[i] = riscv_imsic_create(
            icfg->m_base + (hwaddr)cluster * group_stride
                         + (hwaddr)core_in_cluster * IMSIC_HART_SIZE(0),
            i,              /* ACPU hartid 0-31 */
            true  /* M-mode */,
            1     /* num_pages (no VS) */,
            icfg->num_ids ? icfg->num_ids : 255);
    }

    /*
     * Per-hart S-level IMSIC with guest files (ACPU only).
     * Same per-cluster placement as M-level.
     */
    for (i = 0; i < num_acpu; i++) {
        uint32_t cluster = i / cores_per_cluster;
        uint32_t core_in_cluster = i % cores_per_cluster;
        riscv_imsic_create(
            icfg->s_base + (hwaddr)cluster * group_stride
                         + (hwaddr)core_in_cluster * IMSIC_HART_SIZE(guest_bits),
            i,              /* ACPU hartid 0-31 */
            false /* S-mode */,
            1 + icfg->num_vs_files,
            icfg->num_ids ? icfg->num_ids : 255);
    }

    /* Find APLIC domain configs: root, m, s */
    for (i = 0; i < acfg->num_domains; i++) {
        if (g_strcmp0(acfg->domains[i].level, "root") == 0) {
            root_domain = &acfg->domains[i];
        } else if (g_strcmp0(acfg->domains[i].level, "m") == 0) {
            m_domain = &acfg->domains[i];
        } else if (g_strcmp0(acfg->domains[i].level, "s") == 0) {
            s_domain = &acfg->domains[i];
        }
    }

    /*
     * 3-level APLIC hierarchy (Beta shared between MCPU and ACPU):
     *
     *   MLROOT (direct delivery → MCPU wired interrupts)
     *     └→ MLAPP (MSI → IMSIC M-level, for ACPU M-mode)
     *          └→ SLAPP (MSI → IMSIC S-level, for ACPU S-mode)
     *
     * MLROOT uses direct delivery mode (msimode=false) to serve MCPU.
     * MLAPP and SLAPP use MSI mode to deliver to ACPU via IMSIC.
     * MLROOT delegates all sources to MLAPP.
     */

    /*
     * 3-level APLIC hierarchy:
     *   MLROOT (direct delivery → MCPU, M-mode wired interrupts)
     *     └→ MLAPP (MSI → IMSIC M-level, for ACPU M-mode)
     *          └→ SLAPP (MSI → IMSIC S-level, for ACPU S-mode)
     *
     * MLROOT delegates all sources to MLAPP by default.
     * MCPU management OS can reconfigure sourcecfg to claim specific IRQs.
     */

    /* MLROOT: direct delivery to MCPU (root of hierarchy) */
    DeviceState *aplic_mlroot = NULL;
    if (root_domain && root_domain->base) {
        hwaddr size = root_domain->size ? root_domain->size : 0x10000;
        aplic_mlroot = riscv_aplic_create(
            root_domain->base, size,
            mcpu_hartid_base,  /* hartid_base = 32 (MCPU) */
            num_mcpu,          /* num_harts = 1 */
            num_sources,
            7,
            false,         /* msimode=false: direct delivery */
            true,          /* M-mode */
            NULL           /* root: no parent */);
    }

    /* MLAPP: MSI delivery to IMSIC M-level (child of MLROOT) */
    DeviceState *aplic_mlapp = NULL;
    if (m_domain && m_domain->base) {
        hwaddr size = m_domain->size ? m_domain->size : 0x10000;
        aplic_mlapp = riscv_aplic_create(
            m_domain->base, size,
            0, 0,          /* MSI mode: no direct hart delivery */
            num_sources,
            7,
            true,          /* msimode=true: MSI to IMSIC M */
            true,          /* M-mode */
            aplic_mlroot   /* parent: MLROOT (NULL if root not configured) */);
        s->aplic_m = aplic_mlapp;
    }

    /*
     * Root APLIC has the GPIO inputs for external interrupt sources.
     * With 3-level hierarchy: MLROOT is root.
     * Without MLROOT: MLAPP is root (parent=NULL).
     */
    s->aplic_root = aplic_mlroot ? aplic_mlroot : aplic_mlapp;

    /* SLAPP: MSI delivery to IMSIC S-level (child of MLAPP) */
    if (s_domain && s_domain->base) {
        hwaddr size = s_domain->size ? s_domain->size : 0x10000;
        s->aplic_s = riscv_aplic_create(
            s_domain->base, size,
            0, 0,          /* MSI mode */
            num_sources,
            7,
            true,          /* msimode=true: MSI to IMSIC S */
            false,         /* S-mode */
            aplic_mlapp    /* parent: MLAPP */);
    }
}

/* ---------------------------------------------------------------------------
 * Step 9: UART (ns16550a via serial-mm)
 * -------------------------------------------------------------------------*/

static void beta_create_uart(BetaMachineState *s,
                             MemoryRegion *system_memory)
{
    BetaDeviceConfig *uart = beta_find_device_by_type(s->config, "uart");
    qemu_irq uart_irq = NULL;
    uint32_t clock_hz;
    int irq_num;

    if (!uart || uart->base == 0) {
        return;
    }

    /*
     * Wire UART interrupt to the root (M-mode) APLIC.
     * Only the root APLIC has GPIO inputs; S-mode APLIC gets interrupts
     * delegated from the parent via sourcecfg delegation registers.
     */
    if (uart->irq_name && s->aplic_root) {
        irq_num = beta_find_irq(s->config, uart->irq_name);
        if (irq_num > 0) {
            uart_irq = qdev_get_gpio_in(s->aplic_root, irq_num);
        }
    }

    clock_hz = uart->clock_hz ? uart->clock_hz : 3686400;

    Chardev *chr = serial_hd(0);
    if (!chr) {
        chr = qemu_chr_new("beta-uart0", "stdio", NULL);
    }

    serial_mm_init(system_memory,
                   uart->base,
                   2 /* regshift: 4-byte stride (reg N at offset N*4) */,
                   uart_irq,
                   clock_hz / 16 /* baudbase */,
                   chr,
                   DEVICE_LITTLE_ENDIAN);

    /*
     * The ns16550a with regshift=2 only maps 32 bytes (8 regs × 4 bytes).
     * Some SoC UART drivers access extended registers (DMA/FIFO thresholds)
     * beyond offset 0x1F. Map a RAM stub to absorb those accesses silently
     * so the driver's extended-register init does not fault.
     */
    uint64_t uart_size = uart->size ? uart->size : 0x100;
    if (uart_size > 0x20) {
        MemoryRegion *uart_ext = g_new0(MemoryRegion, 1);
        memory_region_init_ram(uart_ext, NULL, "beta.uart0-ext",
                               uart_size - 0x20, &error_fatal);
        memory_region_add_subregion(system_memory, uart->base + 0x20,
                                    uart_ext);
    }
}

/*
 * =========================================================================
 * APLIC Interrupt Source Wiring Summary
 * =========================================================================
 *
 * The root APLIC (s->aplic_root) has num_sources=1023 GPIO inputs allocated.
 * The production config defines ~99 named interrupt sources in irq_map.
 * Only devices that actively generate interrupts in QEMU need physical
 * wiring to the APLIC.  Sources that exist in the vector table but have
 * no driver never fire, which is correct (the APLIC source remains idle).
 *
 * ACTIVELY WIRED sources (directly connected to aplic_root GPIO inputs):
 *
 *   1. UART (IRQ 5) -- wired above in beta_create_uart() via
 *      qdev_get_gpio_in(s->aplic_root, irq_num).  The ns16550a model
 *      asserts this line on TX/RX events.
 *
 *   2. IOMMU (base IRQs 100, 104, 108, 112 -- 4 per IOMMU instance) --
 *      wired in beta_create_iommu() via the "irqchip" link property.
 *      The RISC-V IOMMU model internally calls qdev_get_gpio_in() on
 *      the linked APLIC using base-irq + offset for fault/queue events.
 *
 *   3. GP Timer (IRQ from irq_map "gp_timer") -- wired in
 *      beta_create_generic_mmio_devices() to aplic_root.  The timer
 *      tick asserts when counter >= compare, deasserts on disable.
 *
 *   4. Mailbox (IRQ from irq_map "mbox_sem") -- wired in
 *      beta_create_generic_mmio_devices() to aplic_root.  Asserts
 *      on TX_DATA write when IRQ is enabled.
 *
 *   5. WDT (IRQ from irq_map "mcpu_wdt") -- wired to MCPU PLIC (not
 *      APLIC).  Asserts on first WDT timeout (RMOD=1), deasserts on
 *      kick or disable.
 *
 * NOT wired (by design -- no active interrupt generation in QEMU):
 *
 *   - PCIe INTx (IRQs 28-91): The DesignWare PCIe host creates 4 INTx
 *     sysbus outputs per controller, but these are legacy PCI interrupt
 *     lines.  On Beta SoC with AIA, PCIe endpoints use MSI/MSI-X which
 *     is delivered directly to IMSIC, bypassing APLIC entirely.  No real
 *     PCIe endpoint is attached in the platform model (config space reads
 *     return 0xFFFFFFFF = no device), so INTx never fires.  If future
 *     work adds PCI endpoint devices that need INTx fallback, wire them
 *     with: sysbus_connect_irq(SYS_BUS_DEVICE(dev), pin,
 *              qdev_get_gpio_in(s->aplic_root, irq_num));
 *
 *   - generic_mmio stubs (i2c, gpio, spi, sensors,
 *     DDR controllers, etc. -- IRQs 1-27, 92-99): Pure register-map
 *     stubs with no interrupt generation logic.  The APLIC source IDs
 *     are reserved so software can enumerate them, but the lines are
 *     never asserted.
 *
 * NOT routed through APLIC (separate interrupt path):
 *
 *   - CLINT/ACLINT (timer + software interrupts): Directly wired to
 *     CPU hart inputs (MIP.MTIP, MIP.MSIP) per RISC-V privilege spec.
 *     Created in beta_create_clint(); does not go through APLIC.
 *
 *   - MCPU PLIC: Separate compact PLIC for MCPU subsystem-internal
 *     interrupts.  Independent of the ACPU AIA (APLIC+IMSIC) hierarchy.
 * =========================================================================
 */

/* ---------------------------------------------------------------------------
 * Step 10: SoC CSR device (beta-soc-csr) with CORE_RELEASE wiring
 * -------------------------------------------------------------------------*/

static void beta_create_soc_csr(BetaMachineState *s,
                                MemoryRegion *system_memory)
{
    BetaDeviceConfig *csr_dev;
    BetaSoCCSRState *csr;
    BetaGenericMMIOState *mmio;
    BetaMMIORegDef *reg_defs = NULL;
    uint32_t num_acpu;
    CPUState **acpu_cores;
    uint32_t i;

    csr_dev = beta_find_device_by_type(s->config, "soc_top_csr");
    if (!csr_dev || csr_dev->base == 0) {
        return;
    }

    num_acpu = s->config->acpu.num_clusters *
               s->config->acpu.cores_per_cluster;

    /* Build ACPU CPUState pointer array */
    acpu_cores = g_new0(CPUState *, num_acpu);
    for (i = 0; i < num_acpu; i++) {
        acpu_cores[i] = CPU(&s->acpu_soc.harts[i]);
    }

    /* Create the device */
    csr = BETA_SOC_CSR(qdev_new(TYPE_BETA_SOC_CSR));
    mmio = BETA_GENERIC_MMIO(csr);
    mmio->device_name = g_strdup(csr_dev->name ? csr_dev->name : "soc-csr");

    /* Set register definitions before realize */
    if (csr_dev->num_registers > 0) {
        reg_defs = beta_convert_reg_defs(csr_dev->registers,
                                         csr_dev->num_registers);
        beta_generic_mmio_set_reg_defs(mmio, reg_defs,
                                       csr_dev->num_registers,
                                       csr_dev->size);
    }

    /*
     * Connect ACPU cores and register names.
     * Production config uses SW_RST_CTRL1 + ACPU_BOOT_ADDR_L_0.
     * Test config may use legacy CORE_RELEASE + BOOT_ADDR.
     * Try production names first, fall back to legacy.
     */
    {
        const char *release_reg = "SW_RST_CTRL1";
        const char *bootaddr_reg = "ACPU_BOOT_ADDR_L_0";
        /* Check if production registers exist, else use legacy */
        bool has_production = false;
        for (uint32_t r = 0; r < csr_dev->num_registers; r++) {
            if (g_strcmp0(csr_dev->registers[r].name, "SW_RST_CTRL1") == 0) {
                has_production = true;
                break;
            }
        }
        if (!has_production) {
            release_reg = "CORE_RELEASE";
            bootaddr_reg = "BOOT_ADDR";
        }
        /*
         * reset_ctrl_reg: legacy test configs use "RESET_CTRL"; production
         * config uses SW_RST_CTRL0 (handled separately in beta_soc_csr.c).
         * Skip if the register doesn't exist to avoid a spurious warning.
         */
        const char *reset_ctrl_reg = NULL;
        for (uint32_t r = 0; r < csr_dev->num_registers; r++) {
            if (g_strcmp0(csr_dev->registers[r].name, "RESET_CTRL") == 0) {
                reset_ctrl_reg = "RESET_CTRL";
                break;
            }
        }
        beta_soc_csr_setup(csr,
                       acpu_cores, num_acpu,
                       s->config->acpu.hartid_base,
                       release_reg,
                       bootaddr_reg,
                       "BOOT_ARG",
                       reset_ctrl_reg,
                       beta_set_acpu_a0,
                       beta_set_acpu_a1);
    }

    /* Wire M-level IMSIC devices for SMP bringup eidelivery pre-enable */
    if (s->acpu_m_imsic && s->num_acpu_m_imsic > 0) {
        beta_soc_csr_set_m_imsic(csr, s->acpu_m_imsic, s->num_acpu_m_imsic);
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(csr), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(csr), 0, csr_dev->base);
    s->soc_csr = csr;  /* Save for fdt_load_addr passthrough */

    /*
     * MCPU IO Space alias: MCPU (RV32) accesses SOC_TOP_CSR at 0xE000_0000
     * which maps to global address 0x01_0000_0000. Create an alias so
     * MCPU writes to 0xE000_0000+offset hit the same MMIO region.
     */
    if (csr_dev->base >= 0x100000000ULL) {
        MemoryRegion *mcpu_alias = g_new(MemoryRegion, 1);
        memory_region_init_alias(mcpu_alias, NULL, "soc-csr-mcpu-alias",
                                 sysbus_mmio_get_region(SYS_BUS_DEVICE(csr), 0),
                                 0, csr_dev->size);
        memory_region_add_subregion(system_memory, 0xE0000000, mcpu_alias);
    }

    g_free(acpu_cores);
    /* reg_defs ownership transferred to device — do NOT free here */
}

/* ---------------------------------------------------------------------------
 * Step 10b: L3 CSR write callbacks (functional behavior for cache flush)
 *
 * In QEMU, all memory writes go directly to RAM — there is no cache
 * hierarchy to flush. So L3 flush/invalidate operations complete
 * instantaneously. The callbacks set FLUSH_INV_DONE so firmware
 * doesn't hang polling for completion.
 * -------------------------------------------------------------------------*/

static void beta_l3_flush_ctrl_cb(void *opaque, const char *reg_name,
                                  uint64_t value)
{
    BetaGenericMMIOState *mmio = BETA_GENERIC_MMIO(opaque);

    if (value & 0x1) {
        /* bit[0] = flush enable → complete immediately, signal done */
        beta_generic_mmio_set_reg(mmio, "L3_FLUSH_INV_DONE", 0x1);
        qemu_log_mask(LOG_UNIMP,
                      "%s: L3_FLUSH_CTRL=0x%" PRIx64
                      " → flush complete (instant in QEMU)\n",
                      mmio->device_name, value);
    }
}

static void beta_l3_inv_event_trig_cb(void *opaque, const char *reg_name,
                                      uint64_t value)
{
    BetaGenericMMIOState *mmio = BETA_GENERIC_MMIO(opaque);

    if (value) {
        /* Any write triggers invalidation → complete immediately */
        beta_generic_mmio_set_reg(mmio, "L3_FLUSH_INV_DONE", 0x1);
        qemu_log_mask(LOG_UNIMP,
                      "%s: L3_INV_EVENT_TRIG=0x%" PRIx64
                      " → invalidate complete (instant in QEMU)\n",
                      mmio->device_name, value);
    }
}

static void beta_l3_ctrl_reg_cb(void *opaque, const char *reg_name,
                                uint64_t value)
{
    BetaGenericMMIOState *mmio = BETA_GENERIC_MMIO(opaque);
    bool bypass = (value & 0x1);
    qemu_log_mask(LOG_UNIMP,
                  "%s: L3_CTRL_REG=0x%" PRIx64 " → %s mode\n",
                  mmio->device_name, value,
                  bypass ? "BYPASS (SF only)" : "NORMAL (cache+SF)");
}

/*
 * L3_CORE_CTRL_REG write callback — bank lock
 *
 * bits[31:0]: per-core L3-way disable bitmask.  Setting a bit disables the
 * corresponding core's access to L3 (cache partitioning / bank lock).
 * No functional effect in QEMU (no cache model); log only.
 */
static void beta_l3_core_ctrl_cb(void *opaque, const char *reg_name,
                                 uint64_t value)
{
    BetaGenericMMIOState *mmio = BETA_GENERIC_MMIO(opaque);
    qemu_log_mask(LOG_UNIMP,
                  "%s: L3_CORE_CTRL_REG=0x%" PRIx64
                  " → bank-lock bitmask (1=core's L3 ways disabled)\n",
                  mmio->device_name, value);
}

/*
 * L3_MEM_CTRL_REG write callback — SPSRAM margin / power gating
 *
 * bits[2:0]=sf_ram_mcs, bits[5:3]=tag_ram_mcs, bits[8:6]=dat_ram_mcs.
 * Writing 0 to any field gates the corresponding SRAM bank.
 * No functional effect in QEMU; log transitions.
 */
static void beta_l3_mem_ctrl_cb(void *opaque, const char *reg_name,
                                uint64_t value)
{
    BetaGenericMMIOState *mmio = BETA_GENERIC_MMIO(opaque);
    unsigned sf_mcs  = (value >> 0) & 0x7;
    unsigned tag_mcs = (value >> 3) & 0x7;
    unsigned dat_mcs = (value >> 6) & 0x7;
    qemu_log_mask(LOG_UNIMP,
                  "%s: L3_MEM_CTRL_REG=0x%" PRIx64
                  " → sf_mcs=%u tag_mcs=%u dat_mcs=%u%s\n",
                  mmio->device_name, value, sf_mcs, tag_mcs, dat_mcs,
                  (sf_mcs == 0 || tag_mcs == 0 || dat_mcs == 0)
                      ? " (WARNING: mcs=0 gates SRAM bank)" : "");
}

/*
 * Attach L3 functional callbacks to a generic_mmio device
 * whose name matches "l3csr*".
 */
static void beta_l3csr_add_callbacks(BetaGenericMMIOState *mmio)
{
    beta_generic_mmio_add_callback(mmio, "L3_FLUSH_CTRL",
                                   beta_l3_flush_ctrl_cb, mmio);
    beta_generic_mmio_add_callback(mmio, "L3_INV_EVENT_TRIG",
                                   beta_l3_inv_event_trig_cb, mmio);
    beta_generic_mmio_add_callback(mmio, "L3_CTRL_REG",
                                   beta_l3_ctrl_reg_cb, mmio);
    beta_generic_mmio_add_callback(mmio, "L3_CORE_CTRL_REG",
                                   beta_l3_core_ctrl_cb, mmio);
    beta_generic_mmio_add_callback(mmio, "L3_MEM_CTRL_REG",
                                   beta_l3_mem_ctrl_cb, mmio);
}

/* ---------------------------------------------------------------------------
 * Step 10b-1b: DDR CSR write callbacks (per-channel DDR controller stub)
 *
 * DWC UMCTL2-compatible register stubs.  Training-poll registers use fixed
 * reset values so firmware exits immediately:
 *   STAT[1:0]  = 1  (Normal mode, init done)         — reset value only
 *   DFISTAT[0] = 1  (dfi_init_complete, PHY ready)   — reset value only
 *
 * SWCTL / SWSTAT: firmware opens a quasi-dynamic write window with SWCTL=0,
 * programs timing registers, then writes SWCTL=1 and polls SWSTAT[0]=1.
 * The callback mirrors SWCTL[0] into SWSTAT immediately so the poll exits.
 * -------------------------------------------------------------------------*/

static void beta_ddr_swctl_cb(void *opaque, const char *reg_name,
                              uint64_t value)
{
    BetaGenericMMIOState *mmio = BETA_GENERIC_MMIO(opaque);
    /* Mirror SWCTL[0] into SWSTAT[0] with zero latency */
    beta_generic_mmio_set_reg(mmio, "SWSTAT", value & 0x1);
    qemu_log_mask(LOG_UNIMP,
                  "%s: SWCTL=0x%" PRIx64 " → SWSTAT=%u (%s)\n",
                  mmio->device_name, value, (unsigned)(value & 0x1),
                  (value & 0x1) ? "quasi-dynamic write committed"
                                : "quasi-dynamic write window open");
}

static void beta_ddr_csr_add_callbacks(BetaGenericMMIOState *mmio)
{
    beta_generic_mmio_add_callback(mmio, "SWCTL",
                                   beta_ddr_swctl_cb, mmio);
}

/* ---------------------------------------------------------------------------
 * Step 10b-2: L2 CSR write callbacks (per-cluster L2 cache flush/inval)
 *
 * Symmetric with L3: in QEMU there is no real L2 cache hierarchy, so
 * flush and invalidate operations complete instantaneously.  Callbacks
 * set L2_FLUSH_DONE so firmware polling loops can exit.
 * Attached to any device whose name starts with "cluster" and ends with
 * "_csr" (cluster0_csr … cluster7_csr).
 * -------------------------------------------------------------------------*/

static void beta_l2_flush_ctrl_cb(void *opaque, const char *reg_name,
                                  uint64_t value)
{
    BetaGenericMMIOState *mmio = BETA_GENERIC_MMIO(opaque);

    if (value & 0x1) {
        /* bit[0] = flush enable → complete immediately, signal done */
        beta_generic_mmio_set_reg(mmio, "L2_FLUSH_DONE", 0x1);
        qemu_log_mask(LOG_UNIMP,
                      "%s: L2_FLUSH_CTRL=0x%" PRIx64
                      " → L2 flush complete (instant in QEMU)\n",
                      mmio->device_name, value);
    }
}

static void beta_l2_inv_event_trig_cb(void *opaque, const char *reg_name,
                                      uint64_t value)
{
    BetaGenericMMIOState *mmio = BETA_GENERIC_MMIO(opaque);

    if (value) {
        /* Any write triggers invalidation → complete immediately */
        beta_generic_mmio_set_reg(mmio, "L2_FLUSH_DONE", 0x1);
        qemu_log_mask(LOG_UNIMP,
                      "%s: L2_INV_EVENT_TRIG=0x%" PRIx64
                      " → L2 invalidate complete (instant in QEMU)\n",
                      mmio->device_name, value);
    }
}

/*
 * Attach L2 functional callbacks to a generic_mmio device
 * whose name matches "cluster*_csr".
 */
static void beta_l2csr_add_callbacks(BetaGenericMMIOState *mmio)
{
    beta_generic_mmio_add_callback(mmio, "L2_FLUSH_CTRL",
                                   beta_l2_flush_ctrl_cb, mmio);
    beta_generic_mmio_add_callback(mmio, "L2_INV_EVENT_TRIG",
                                   beta_l2_inv_event_trig_cb, mmio);
}

/* ---------------------------------------------------------------------------
 * Step 10c: WDT write callbacks (DesignWare WDT functional behavior)
 *
 * Minimal DesignWare WDT emulation:
 * - WDT_CCVR returns a value derived from QEMU virtual time so firmware
 *   sees a "counting" watchdog.
 * - WDT_CRR: writing 0x76 restarts the counter (standard DW WDT kick).
 * - WDT_EOI: read-to-clear clears interrupt status.
 * -------------------------------------------------------------------------*/

static void beta_wdt_crr_cb(void *opaque, const char *reg_name,
                             uint64_t value)
{
    BetaGenericMMIOState *mmio = BETA_GENERIC_MMIO(opaque);

    if ((value & 0xFF) == 0x76) {
        /* Standard DW WDT restart magic value → reset counter to max */
        beta_generic_mmio_set_reg(mmio, "WDT_CCVR", 0x0000FFFF);
        beta_generic_mmio_set_reg(mmio, "WDT_STAT", 0x0);
    }
}

static void beta_wdt_add_callbacks(BetaGenericMMIOState *mmio)
{
    beta_generic_mmio_add_callback(mmio, "WDT_CRR",
                                   beta_wdt_crr_cb, mmio);
}

/* ---------------------------------------------------------------------------
 * Step 10d: TRNG write callback (True Random Number Generator)
 *
 * Writing to TRNG_CTRL generates a new random word and stores it in
 * TRNG_DATA. TRNG_STATUS always reads 0x1 (data ready).
 * -------------------------------------------------------------------------*/

static void beta_trng_ctrl_cb(void *opaque, const char *reg_name,
                               uint64_t value)
{
    BetaGenericMMIOState *mmio = BETA_GENERIC_MMIO(opaque);
    /* Generate new random word and store in TRNG_DATA */
    uint32_t rnd;
    qemu_guest_getrandom_nofail(&rnd, sizeof(rnd));
    beta_generic_mmio_set_reg(mmio, "TRNG_DATA", rnd);
    beta_generic_mmio_set_reg(mmio, "TRNG_STATUS", 0x1);
}

static void beta_trng_add_callbacks(BetaGenericMMIOState *mmio)
{
    beta_generic_mmio_add_callback(mmio, "TRNG_CTRL",
                                   beta_trng_ctrl_cb, mmio);
}

/* ---------------------------------------------------------------------------
 * Step 10e: GP Timer write callbacks (functional timer with compare match)
 *
 * Uses a QEMU virtual timer to periodically increment the 64-bit counter
 * (CNT_H:CNT_L). When the counter reaches or exceeds the compare value
 * (CMP_H:CMP_L), the timer fires and logs the event.
 * CTRL bit[0] = enable: starts/stops the periodic tick.
 * Timer tick interval: 1 ms (NANOSECONDS_PER_SECOND / 1000).
 * -------------------------------------------------------------------------*/

/* GP Timer tick interval: 1 ms */
#define GP_TIMER_TICK_NS  (NANOSECONDS_PER_SECOND / 1000)

typedef struct BetaGPTimerState {
    BetaGenericMMIOState *mmio;
    QEMUTimer *timer;
    qemu_irq irq;  /* APLIC interrupt line (NULL if not wired) */
} BetaGPTimerState;

static void beta_gp_timer_tick(void *opaque)
{
    BetaGPTimerState *ts = (BetaGPTimerState *)opaque;
    BetaGenericMMIOState *mmio = ts->mmio;
    uint64_t cnt_l, cnt_h, cmp_l, cmp_h;
    uint64_t cnt, cmp;

    cnt_l = beta_generic_mmio_get_reg(mmio, "GP_TIMER_CNT_L");
    cnt_h = beta_generic_mmio_get_reg(mmio, "GP_TIMER_CNT_H");
    cmp_l = beta_generic_mmio_get_reg(mmio, "GP_TIMER_CMP_L");
    cmp_h = beta_generic_mmio_get_reg(mmio, "GP_TIMER_CMP_H");

    cnt = ((uint64_t)cnt_h << 32) | (cnt_l & 0xFFFFFFFF);
    cmp = ((uint64_t)cmp_h << 32) | (cmp_l & 0xFFFFFFFF);

    cnt++;

    /* Store back split 32-bit halves */
    beta_generic_mmio_set_reg(mmio, "GP_TIMER_CNT_L",
                              (uint32_t)(cnt & 0xFFFFFFFF));
    beta_generic_mmio_set_reg(mmio, "GP_TIMER_CNT_H",
                              (uint32_t)(cnt >> 32));

    if (cnt >= cmp) {
        /* Timer fired: compare match reached — assert IRQ */
        if (ts->irq) {
            qemu_set_irq(ts->irq, 1);
        }
        qemu_log_mask(LOG_UNIMP,
                      "%s: GP Timer fired (CNT=0x%" PRIx64
                      " >= CMP=0x%" PRIx64 ")\n",
                      mmio->device_name, cnt, cmp);
    }

    /* Re-arm if still enabled */
    if (beta_generic_mmio_get_reg(mmio, "GP_TIMER_CTRL") & 0x1) {
        timer_mod(ts->timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + GP_TIMER_TICK_NS);
    }
}

static void beta_gp_timer_ctrl_cb(void *opaque, const char *reg_name,
                                   uint64_t value)
{
    BetaGPTimerState *ts = (BetaGPTimerState *)opaque;

    if (value & 0x1) {
        /* Enable: start periodic tick */
        timer_mod(ts->timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + GP_TIMER_TICK_NS);
        qemu_log_mask(LOG_UNIMP,
                      "%s: GP Timer enabled\n",
                      ts->mmio->device_name);
    } else {
        /* Disable: stop the tick and deassert IRQ */
        timer_del(ts->timer);
        if (ts->irq) {
            qemu_set_irq(ts->irq, 0);
        }
        qemu_log_mask(LOG_UNIMP,
                      "%s: GP Timer disabled\n",
                      ts->mmio->device_name);
    }
}

static void beta_gp_timer_add_callbacks(BetaGenericMMIOState *mmio,
                                        qemu_irq irq)
{
    BetaGPTimerState *ts = g_new0(BetaGPTimerState, 1);

    ts->mmio = mmio;
    ts->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, beta_gp_timer_tick, ts);
    ts->irq = irq;

    beta_generic_mmio_add_callback(mmio, "GP_TIMER_CTRL",
                                   beta_gp_timer_ctrl_cb, ts);
}

/* ---------------------------------------------------------------------------
 * Step 10f: WDT enhanced callbacks (timer-based counter decrement)
 *
 * Extends the basic WDT_CRR kick with a periodic timer that decrements
 * WDT_CCVR. When the counter reaches 0:
 *   - RMOD=0 (WDT_CR bit[1]=0): trigger system reset
 *   - RMOD=1 (WDT_CR bit[1]=1): set WDT_STAT bit[0]=1 (interrupt phase),
 *     then start a second timeout period before reset.
 *
 * WDT_CR write: bit[0] transitions enable/disable the timer.
 * WDT_CRR write (0x76): restarts counter and timer period.
 * Timer period: fixed 1-second approximation of DW WDT timeout.
 * -------------------------------------------------------------------------*/

#define WDT_TIMEOUT_NS  NANOSECONDS_PER_SECOND  /* 1-second tick */

typedef struct BetaWDTTimerState {
    BetaGenericMMIOState *mmio;
    QEMUTimer *timer;
    bool second_timeout;  /* true if in interrupt-to-reset phase (RMOD=1) */
    qemu_irq irq;  /* MCPU PLIC interrupt line (NULL if not wired) */
} BetaWDTTimerState;

static void beta_wdt_timer_tick(void *opaque)
{
    BetaWDTTimerState *ws = (BetaWDTTimerState *)opaque;
    BetaGenericMMIOState *mmio = ws->mmio;
    uint64_t ccvr, cr;

    cr = beta_generic_mmio_get_reg(mmio, "WDT_CR");

    /* Check if WDT is still enabled */
    if (!(cr & 0x1)) {
        return;
    }

    ccvr = beta_generic_mmio_get_reg(mmio, "WDT_CCVR");

    if (ccvr > 0) {
        ccvr--;
        beta_generic_mmio_set_reg(mmio, "WDT_CCVR", ccvr);
    }

    if (ccvr == 0) {
        uint64_t rmod = (cr >> 1) & 0x1;

        if (rmod == 0) {
            /* RMOD=0: system reset immediately */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: WDT timeout (RMOD=0) — system reset!\n",
                          mmio->device_name);
            qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
            return;
        }

        /* RMOD=1: interrupt phase */
        if (!ws->second_timeout) {
            /* First timeout: raise interrupt status, start second period */
            beta_generic_mmio_set_reg(mmio, "WDT_STAT", 0x1);
            ws->second_timeout = true;
            if (ws->irq) {
                qemu_set_irq(ws->irq, 1);
            }
            qemu_log_mask(LOG_UNIMP,
                          "%s: WDT first timeout (RMOD=1) — interrupt set, "
                          "starting second timeout\n",
                          mmio->device_name);
            /* Reload counter for second timeout */
            beta_generic_mmio_set_reg(mmio, "WDT_CCVR", 0x0000FFFF);
            timer_mod(ws->timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + WDT_TIMEOUT_NS);
            return;
        }

        /* Second timeout expired without kick: system reset */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: WDT second timeout (RMOD=1) — system reset!\n",
                      mmio->device_name);
        qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
        return;
    }

    /* Counter still positive: re-arm */
    timer_mod(ws->timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + WDT_TIMEOUT_NS);
}

static void beta_wdt_cr_cb(void *opaque, const char *reg_name,
                            uint64_t value)
{
    BetaWDTTimerState *ws = (BetaWDTTimerState *)opaque;
    BetaGenericMMIOState *mmio = ws->mmio;

    if (value & 0x1) {
        /*
         * WDT enable: load initial counter from WDT_TORR and start timer.
         * DW WDT uses 2^(16 + TOP) cycles; we approximate with the raw
         * TORR[3:0] field as a starting counter value for QEMU.
         */
        uint64_t torr = beta_generic_mmio_get_reg(mmio, "WDT_TORR");
        uint32_t top = torr & 0xF;
        uint64_t initial_count = 1ULL << (16 + top);

        /* Clamp to 32-bit register width */
        if (initial_count > 0xFFFFFFFF) {
            initial_count = 0xFFFFFFFF;
        }

        beta_generic_mmio_set_reg(mmio, "WDT_CCVR", initial_count);
        ws->second_timeout = false;

        timer_mod(ws->timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + WDT_TIMEOUT_NS);
        qemu_log_mask(LOG_UNIMP,
                      "%s: WDT enabled (TOP=%u, initial count=0x%" PRIx64 ")\n",
                      mmio->device_name, top, initial_count);
    } else {
        /* WDT disable: stop the timer and deassert IRQ */
        timer_del(ws->timer);
        ws->second_timeout = false;
        if (ws->irq) {
            qemu_set_irq(ws->irq, 0);
        }
        qemu_log_mask(LOG_UNIMP,
                      "%s: WDT disabled\n", mmio->device_name);
    }
}

static void beta_wdt_crr_enhanced_cb(void *opaque, const char *reg_name,
                                      uint64_t value)
{
    BetaWDTTimerState *ws = (BetaWDTTimerState *)opaque;
    BetaGenericMMIOState *mmio = ws->mmio;

    if ((value & 0xFF) == 0x76) {
        /* Standard DW WDT restart magic value: reset counter and state */
        beta_generic_mmio_set_reg(mmio, "WDT_CCVR", 0x0000FFFF);
        beta_generic_mmio_set_reg(mmio, "WDT_STAT", 0x0);
        ws->second_timeout = false;
        if (ws->irq) {
            qemu_set_irq(ws->irq, 0);
        }

        /* Restart the timer period if WDT is enabled */
        if (beta_generic_mmio_get_reg(mmio, "WDT_CR") & 0x1) {
            timer_mod(ws->timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + WDT_TIMEOUT_NS);
        }
    }
}

static void beta_wdt_enhanced_add_callbacks(BetaGenericMMIOState *mmio,
                                            qemu_irq irq)
{
    BetaWDTTimerState *ws = g_new0(BetaWDTTimerState, 1);

    ws->mmio = mmio;
    ws->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, beta_wdt_timer_tick, ws);
    ws->second_timeout = false;
    ws->irq = irq;

    /*
     * Override the basic WDT_CRR callback with the enhanced one that also
     * restarts the timer, and add WDT_CR for enable/disable control.
     */
    beta_generic_mmio_add_callback(mmio, "WDT_CRR",
                                   beta_wdt_crr_enhanced_cb, ws);
    beta_generic_mmio_add_callback(mmio, "WDT_CR",
                                   beta_wdt_cr_cb, ws);
}

/* ---------------------------------------------------------------------------
 * Step 10g: Mailbox (mbox_sem) write callbacks
 *
 * Simple loopback mailbox:
 * - Writing MBOX_TX_DATA copies the value to MBOX_RX_DATA and sets
 *   MBOX_RX_STATUS=1 (data available).
 * - If MBOX_IRQ_ENABLE bit[0] is set, MBOX_IRQ_STATUS bit[0] is asserted.
 * - MBOX_IRQ_STATUS is W1C (handled by generic_mmio W1C access type).
 * - MBOX_TX_STATUS stays 1 (always ready, instant loopback in QEMU).
 * -------------------------------------------------------------------------*/

typedef struct BetaMboxState {
    BetaGenericMMIOState *mmio;
    qemu_irq irq;  /* APLIC interrupt line (NULL if not wired) */
} BetaMboxState;

static void beta_mbox_tx_data_cb(void *opaque, const char *reg_name,
                                  uint64_t value)
{
    BetaMboxState *ms = (BetaMboxState *)opaque;
    BetaGenericMMIOState *mmio = ms->mmio;

    /* Copy TX data to RX data register (loopback) */
    beta_generic_mmio_set_reg(mmio, "MBOX_RX_DATA", value);

    /* Mark RX as having data available */
    beta_generic_mmio_set_reg(mmio, "MBOX_RX_STATUS", 0x1);

    /* TX remains ready (instant loopback in QEMU) */
    beta_generic_mmio_set_reg(mmio, "MBOX_TX_STATUS", 0x1);

    /* If IRQ enabled, assert IRQ status and APLIC line */
    if (beta_generic_mmio_get_reg(mmio, "MBOX_IRQ_ENABLE") & 0x1) {
        beta_generic_mmio_set_reg(mmio, "MBOX_IRQ_STATUS", 0x1);
        if (ms->irq) {
            qemu_set_irq(ms->irq, 1);
        }
    }

    qemu_log_mask(LOG_UNIMP,
                  "%s: TX_DATA=0x%" PRIx64 " -> loopback to RX_DATA\n",
                  mmio->device_name, value);
}

static void beta_mbox_add_callbacks(BetaGenericMMIOState *mmio,
                                    qemu_irq irq)
{
    BetaMboxState *ms = g_new0(BetaMboxState, 1);
    ms->mmio = mmio;
    ms->irq = irq;

    beta_generic_mmio_add_callback(mmio, "MBOX_TX_DATA",
                                   beta_mbox_tx_data_cb, ms);
}

/* ---------------------------------------------------------------------------
 * Step 11: Generic MMIO devices (type == "generic_mmio")
 * -------------------------------------------------------------------------*/

/*
 * Return true if [dev_base, dev_base+dev_size) overlaps any APLIC domain or
 * any IMSIC region registered by beta_create_aia().  Generic MMIO stubs that
 * overlap these regions must be skipped: QEMU's memory model gives priority to
 * the last-mapped region at equal priority, so a later generic_mmio stub would
 * shadow the real emulation and silently swallow all reads/writes.
 */
static bool beta_mmio_overlaps_hw(BetaSoCConfig *cfg,
                                  hwaddr dev_base, hwaddr dev_size)
{
    hwaddr dev_end = dev_base + dev_size;
    uint32_t j;

    /* APLIC domains */
    BetaAPLICConfig *acfg = &cfg->interrupts.aplic;
    for (j = 0; j < acfg->num_domains; j++) {
        hwaddr a_base = acfg->domains[j].base;
        hwaddr a_size = acfg->domains[j].size ? acfg->domains[j].size : 0x10000;
        if (dev_base < a_base + a_size && dev_end > a_base) {
            return true;
        }
    }

    /* IMSIC regions (per-cluster M-level and S-level) */
    BetaIMSICConfig *icfg = &cfg->interrupts.imsic;
    if (icfg->m_base && icfg->s_base) {
        uint32_t num_clusters = cfg->acpu.num_clusters;
        uint32_t cores_per_cluster = cfg->acpu.cores_per_cluster;
        uint32_t guest_bits = icfg->num_vs_files ?
                              beta_num_bits(icfg->num_vs_files + 1) : 0;
        hwaddr group_stride   = (hwaddr)1 << IMSIC_MMIO_GROUP_MIN_SHIFT;
        hwaddr m_cluster_size = (hwaddr)cores_per_cluster * IMSIC_HART_SIZE(0);
        hwaddr s_cluster_size = (hwaddr)cores_per_cluster *
                                IMSIC_HART_SIZE(guest_bits);

        for (j = 0; j < num_clusters; j++) {
            hwaddr m_base = icfg->m_base + (hwaddr)j * group_stride;
            hwaddr s_base = icfg->s_base + (hwaddr)j * group_stride;
            if ((dev_base < m_base + m_cluster_size && dev_end > m_base) ||
                (dev_base < s_base + s_cluster_size && dev_end > s_base)) {
                return true;
            }
        }
    }

    return false;
}

static void beta_create_generic_mmio_devices(BetaMachineState *s,
                                             MemoryRegion *system_memory)
{
    BetaSoCConfig *cfg = s->config;
    uint32_t i;

    for (i = 0; i < cfg->num_devices; i++) {
        BetaDeviceConfig *dev = &cfg->devices[i];
        BetaGenericMMIOState *mmio;
        BetaMMIORegDef *reg_defs;

        if (g_strcmp0(dev->type, "generic_mmio") != 0) {
            continue;
        }
        if (dev->base == 0) {
            continue;
        }

        /*
         * Skip generic_mmio stubs whose address range overlaps any APLIC
         * domain or IMSIC region.  beta_create_aia() registers those regions
         * first; QEMU's memory model gives priority to the last-mapped region
         * at equal priority, so a later generic_mmio stub would shadow the
         * real emulation and silently swallow all reads/writes.
         */
        if (beta_mmio_overlaps_hw(cfg, dev->base, dev->size)) {
            qemu_log_mask(LOG_UNIMP,
                          "beta: skipping generic_mmio \"%s\" "
                          "@ 0x%"PRIx64"+0x%"PRIx64
                          ": overlaps APLIC/IMSIC emulation region\n",
                          dev->name ? dev->name : "(unnamed)",
                          dev->base, dev->size);
            continue;
        }

        mmio = BETA_GENERIC_MMIO(qdev_new(TYPE_BETA_GENERIC_MMIO));
        mmio->device_name = g_strdup(dev->name ? dev->name : "generic-mmio");

        if (dev->num_registers > 0) {
            reg_defs = beta_convert_reg_defs(dev->registers,
                                             dev->num_registers);
            beta_generic_mmio_set_reg_defs(mmio, reg_defs,
                                           dev->num_registers, dev->size);
            /* reg_defs ownership transferred to device */
        } else if (dev->size > 0) {
            /* No registers defined: create a dummy single-register placeholder */
            beta_generic_mmio_set_reg_defs(mmio, NULL, 0, dev->size);
        }

        sysbus_realize_and_unref(SYS_BUS_DEVICE(mmio), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(mmio), 0, dev->base);

        /*
         * Post-creation hooks: add functional callbacks for specific devices.
         * The generic_mmio framework supports per-register write callbacks
         * that fire after the value is stored.
         */
        if (dev->name && g_str_has_prefix(dev->name, "l3csr")) {
            beta_l3csr_add_callbacks(mmio);
        }
        if (dev->name && g_str_has_prefix(dev->name, "cluster") &&
            g_str_has_suffix(dev->name, "_csr")) {
            beta_l2csr_add_callbacks(mmio);
        }
        if (dev->name && g_strcmp0(dev->name, "wdt") == 0) {
            /*
             * Use the enhanced WDT callbacks which include timer-based
             * counter decrement and WDT_CR enable/disable control.
             * This supersedes beta_wdt_add_callbacks() — the enhanced
             * version registers its own WDT_CRR handler that also
             * restarts the timer.
             *
             * WDT IRQ goes to MCPU PLIC (mcpu_wdt is IRQ #1 in the
             * MCPU subsystem interrupt controller).
             */
            {
                qemu_irq wdt_irq = NULL;
                if (s->mcpu_plic_irqs && dev->irq_name) {
                    int irq_num = beta_find_irq(cfg, dev->irq_name);
                    if (irq_num > 0 && irq_num < MCPU_PLIC_NUM_SOURCES) {
                        wdt_irq = s->mcpu_plic_irqs[irq_num];
                    }
                }
                beta_wdt_enhanced_add_callbacks(mmio, wdt_irq);
            }
        }
        if (dev->name && g_strcmp0(dev->name, "trng") == 0) {
            beta_trng_add_callbacks(mmio);
        }
        if (dev->name && g_strcmp0(dev->name, "gp_timer") == 0) {
            /*
             * GP Timer IRQ goes to root APLIC.
             */
            {
                qemu_irq gp_irq = NULL;
                if (s->aplic_root && dev->irq_name) {
                    int irq_num = beta_find_irq(cfg, dev->irq_name);
                    if (irq_num > 0) {
                        gp_irq = qdev_get_gpio_in(s->aplic_root, irq_num);
                    }
                }
                beta_gp_timer_add_callbacks(mmio, gp_irq);
            }
        }
        if (dev->name && g_strcmp0(dev->name, "mbox_sem") == 0) {
            /*
             * Mailbox IRQ goes to root APLIC.
             */
            {
                qemu_irq mbox_irq = NULL;
                if (s->aplic_root && dev->irq_name) {
                    int irq_num = beta_find_irq(cfg, dev->irq_name);
                    if (irq_num > 0) {
                        mbox_irq = qdev_get_gpio_in(s->aplic_root, irq_num);
                    }
                }
                beta_mbox_add_callbacks(mmio, mbox_irq);
            }
        }
        if (dev->name && g_str_has_prefix(dev->name, "ddr") &&
            g_str_has_suffix(dev->name, "_csr")) {
            beta_ddr_csr_add_callbacks(mmio);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Step 12: NOR Flash (pflash_cfi01)
 * -------------------------------------------------------------------------*/

static void beta_create_flash(BetaMachineState *s)
{
    BetaBootConfig *boot = &s->config->boot;
    DriveInfo *dinfo;

    if (boot->flash_base == 0 || boot->flash_size == 0) {
        return;
    }

    dinfo = drive_get(IF_PFLASH, 0, 0);

    s->flash = pflash_cfi01_register(
        boot->flash_base,
        "beta.flash",
        boot->flash_size,
        dinfo ? blk_by_legacy_dinfo(dinfo) : NULL,
        64 * KiB    /* sector size */,
        4           /* width (32-bit) */,
        0x00, 0x89, /* manufacturer/device IDs (Intel-compatible) */
        0x00, 0x00,
        0           /* little-endian */);
}

/* ---------------------------------------------------------------------------
 * Step 12b: PCIe (Synopsys DesignWare PCIe host controllers)
 * -------------------------------------------------------------------------*/

/*
 * PCIe config space fallback handler.
 *
 * Bus 0 config is accessed through DBI (designware_pcie_host_mmio_ops),
 * so this handler only sees bus 1+ accesses.  Since no real devices
 * exist behind the root port, every read returns all-1s ("no device")
 * and writes are silently dropped.
 *
 * Mapped at ecam_base with priority -1 so the DWC outbound ATU cfg
 * viewport (priority 0) takes precedence when it is active.
 */


/*
 * APPL register block: APPL_RV_AHB_OVRD_REG_01 (offset 0x4) always
 * reports link-up.  All other offsets are stored in a backing buffer.
 */
typedef struct BetaPCIeApplState {
    uint8_t *buf;
    uint32_t size;
} BetaPCIeApplState;

static uint64_t beta_pcie_appl_read(void *opaque, hwaddr addr, unsigned size)
{
    BetaPCIeApplState *s = opaque;
    uint64_t val = 0;
    if (addr + size <= s->size) {
        memcpy(&val, &s->buf[addr], size);
    }
    /* Ensure link-up bits are always set in REG_01 (offset 0x4) */
    if (addr <= 0x4 && addr + size > 0x4) {
        unsigned off = 0x4 - addr;
        uint32_t linkup = (1U << 12) | (1U << 1);
        uint32_t existing;
        memcpy(&existing, &((uint8_t *)&val)[off],
               MIN(size - off, sizeof(existing)));
        existing |= linkup;
        memcpy(&((uint8_t *)&val)[off], &existing,
               MIN(size - off, sizeof(existing)));
    }
    return val;
}

static void beta_pcie_appl_write(void *opaque, hwaddr addr,
                                 uint64_t val, unsigned size)
{
    BetaPCIeApplState *s = opaque;
    if (addr + size <= s->size) {
        memcpy(&s->buf[addr], &val, size);
    }
}

static const MemoryRegionOps beta_pcie_appl_ops = {
    .read  = beta_pcie_appl_read,
    .write = beta_pcie_appl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl  = { .min_access_size = 4, .max_access_size = 4 },
};

/*
 * PCIe config space handler: ECAM-style decode.
 * addr encodes bus/dev/fn/reg: addr = (bus << 20) | (devfn << 12) | reg
 * opaque = PCIHostState of the DWC controller.
 */
static uint64_t beta_pcie_config_read(void *opaque, hwaddr addr, unsigned size)
{
    PCIHostState *pci = opaque;
    if (!pci) {
        return UINT64_MAX;
    }
    int bus_num = (addr >> 20) & 0xFF;
    int devfn   = (addr >> 12) & 0xFF;
    int reg     = addr & 0xFFF;
    PCIBus *bus = pci_find_bus_nr(pci->bus, bus_num);
    if (!bus) {
        return UINT64_MAX;
    }
    PCIDevice *dev = pci_find_device(bus, bus_num, devfn);
    if (!dev) {
        return UINT64_MAX;
    }
    return pci_host_config_read_common(dev, reg,
                                       pci_config_size(dev), size);
}

static void beta_pcie_config_write(void *opaque, hwaddr addr,
                                   uint64_t val, unsigned size)
{
    PCIHostState *pci = opaque;
    if (!pci) {
        return;
    }
    int bus_num = (addr >> 20) & 0xFF;
    int devfn   = (addr >> 12) & 0xFF;
    int reg     = addr & 0xFFF;
    PCIBus *bus = pci_find_bus_nr(pci->bus, bus_num);
    if (!bus) {
        return;
    }
    PCIDevice *dev = pci_find_device(bus, bus_num, devfn);
    if (dev) {
        pci_host_config_write_common(dev, reg,
                                      pci_config_size(dev), val, size);
    }
}

static const MemoryRegionOps beta_pcie_config_ops = {
    .read  = beta_pcie_config_read,
    .write = beta_pcie_config_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void beta_create_pcie(BetaMachineState *s,
                              MemoryRegion *system_memory)
{
    BetaSoCConfig *cfg = s->config;
    uint32_t i;

    for (i = 0; i < cfg->num_pcie; i++) {
        BetaPCIeConfig *pcfg = &cfg->pcie[i];
        int num_ctrl = (pcfg->bifurcation == 4) ? 4 : 1;
        int j;

        if (pcfg->dbi_base == 0) {
            continue;
        }

        for (j = 0; j < num_ctrl; j++) {
            DeviceState *dev;

            /*
             * In x16 mode (bifurcation=1): single DWC controller at dbi_base.
             * In 4x4 mode (bifurcation=4): 4 independent DWC controllers,
             * each with 256KB DBI space at dbi_base + j * 0x40000.
             */
            hwaddr ctrl_dbi_base = pcfg->dbi_base + j * 0x40000;

            /*
             * Create Synopsys DesignWare PCIe host controller.
             *
             * The DWC model implements:
             *   - DBI register space (iATU viewport, MSI, link status)
             *   - Outbound ATU: CPU → PCI address translation via aliases
             *   - Inbound ATU: PCI → CPU (DMA) address translation
             *   - Config space access via ATU CFG viewport
             *
             * Linux pcie-designware driver programs iATU through DBI
             * config writes. ECAM and MMIO access go through the ATU
             * aliases, which means iATU translation is applied even
             * for VFIO devices (when mmap fast path is disabled).
             */
            dev = qdev_new(TYPE_DESIGNWARE_PCIE_HOST);
            /*
             * Clear vmsd on host and root to avoid vmstate compat assertion
             * when creating multiple DWC instances. We don't need VM migration.
             */
            DEVICE_GET_CLASS(dev)->vmsd = NULL;
            sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

            /* Record host device so beta_create_iommu() can wire it later. */
            if (i < 4 && j < 4) {
                s->pcie_host_devs[i][j] = dev;
                s->pcie_num_ctrl[i] = j + 1;
            }

            {
                DesignwarePCIEHost *host = DESIGNWARE_PCIE_HOST(dev);
                DEVICE_GET_CLASS(DEVICE(&host->root))->vmsd = NULL;

                /*
                 * Map system memory into PCI address space for DMA + MSI-X.
                 * virtio-pci devices DMA through PCI address space; MSI-X
                 * writes target IMSIC addresses in system memory.
                 * Use an alias since system_memory already has a container.
                 */
                {
                    MemoryRegion *dma = g_new(MemoryRegion, 1);
                    memory_region_init_alias(dma, NULL, "pcie-dma",
                                            system_memory, 0, UINT64_MAX);
                    memory_region_add_subregion_overlap(
                        &host->pci.address_space_root, 0x0, dma, 0);
                }
            }

            /* Map DBI register space at the controller's base address */
            sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, ctrl_dbi_base);

            /*
             * Create dummy MMIO regions for mgmt, ATU, and appl areas
             * that the rv-gen4 driver probes. Without these, accessing
             * unmapped addresses causes bus errors and probe failure.
             *
             * In 4x4 mode:
             *   mgmt: shared per-subsystem at dbi_base + 0x5000000
             *   atu:  per-controller within DBI range
             *   appl: per-controller at dbi_base + 0x6000000 + j * 0x200000
             *   ecam: total ecam_size / 4 per controller
             *
             * In x16 mode: single set of regions (j==0).
             */
            {
                hwaddr appl_base, appl_size;
                hwaddr cfg_base, cfg_size;

                if (num_ctrl == 4) {
                    appl_base = pcfg->dbi_base + 0x6000000 + j * 0x200000;
                    appl_size = 0x200000;    /* 2MB per controller APPL */
                    cfg_base  = pcfg->ecam_base +
                                j * (pcfg->ecam_size / 4);
                    cfg_size  = pcfg->ecam_size ? pcfg->ecam_size / 4
                                                : 0x4000000;
                } else {
                    appl_base = pcfg->dbi_base + 0x6000000;
                    appl_size = 0x800000;
                    cfg_base  = pcfg->ecam_base;
                    cfg_size  = pcfg->ecam_size ? pcfg->ecam_size
                                                : 0x10000000;
                }

                /* Mgmt is per-subsystem; only create once (j==0) */
                if (j == 0) {
                    hwaddr mgmt_base = pcfg->dbi_base + 0x5000000;
                    MemoryRegion *mgmt_mr = g_new(MemoryRegion, 1);
                    g_autofree char *mn =
                        g_strdup_printf("pcie%u-mgmt", i);
                    memory_region_init_ram(mgmt_mr, NULL, mn,
                                          0x100000, &error_fatal);
                    memory_region_add_subregion(system_memory,
                                                mgmt_base, mgmt_mr);
                }

                {
                    MemoryRegion *appl_mr = g_new(MemoryRegion, 1);
                    MemoryRegion *cfg_mr  = g_new(MemoryRegion, 1);
                    g_autofree char *pn =
                        g_strdup_printf("pcie%u.%d-appl", i, j);
                    g_autofree char *cn =
                        g_strdup_printf("pcie%u.%d-config", i, j);
                    {
                        BetaPCIeApplState *as = g_new0(BetaPCIeApplState, 1);
                        as->size = appl_size;
                        as->buf = g_malloc0(appl_size);
                        memory_region_init_io(appl_mr, NULL,
                                              &beta_pcie_appl_ops,
                                              as, pn, appl_size);
                    }

                    /*
                     * Config space fallback: MMIO that returns 0xFFFFFFFF
                     * for all reads (no device present).  Priority -1 so
                     * the DWC outbound ATU cfg viewport wins when active.
                     */
                    memory_region_init_io(cfg_mr, NULL,
                                          &beta_pcie_config_ops,
                                          PCI_HOST_BRIDGE(dev),
                                          cn, cfg_size);
                    memory_region_add_subregion_overlap(system_memory,
                                                        cfg_base,
                                                        cfg_mr, -1);
                    /* ATU registers are within DBI space, handled by DWC model */
                    memory_region_add_subregion(system_memory,
                                                appl_base, appl_mr);

                    /* Link-up bits always set by beta_pcie_appl_ops */
                }
            }

            /* Wire INTx (pins 0-3) from DWC to root APLIC */
            if (s->aplic_root && pcfg->irq_intx_name) {
                int intx_base = beta_find_irq(s->config,
                                              pcfg->irq_intx_name);
                if (intx_base > 0) {
                    for (int pin = 0; pin < 4; pin++) {
                        sysbus_connect_irq(SYS_BUS_DEVICE(dev), pin,
                            qdev_get_gpio_in(s->aplic_root,
                                             intx_base + pin));
                    }
                }
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Step 12c: RISC-V IOMMU (one per PCIe subsystem)
 *
 * Wire each IOMMU to all DWC controllers in its subsystem so that PCIe device
 * DMA passes through RISC-V IOMMU translation.
 *
 * Two-pass approach:
 *   Pass 1 – realize all IOMMU devices.  riscv_iommu_sys_realize() performs
 *             an opportunistic object_resolve_path_type() to auto-connect to
 *             whatever PCI bus it finds first; this may be incorrect when
 *             multiple subsystems exist.
 *   Pass 2 – explicitly call riscv_iommu_sys_setup_pci_bus() for every DWC
 *             controller bus in each subsystem.  Running after all IOMMUs are
 *             realized ensures the final pci_setup_iommu() binding on every
 *             bus is correct, overriding any wrong auto-discovery from pass 1.
 * -------------------------------------------------------------------------*/

static void beta_create_iommu(BetaMachineState *s)
{
    BetaSoCConfig *cfg = s->config;
    DeviceState *iommu_devs[4] = { NULL };
    uint32_t i;

    /* Pass 1: create and realize all IOMMU platform devices. */
    for (i = 0; i < cfg->num_pcie && i < 4; i++) {
        BetaPCIeConfig *pcfg = &cfg->pcie[i];
        DeviceState *dev;

        if (pcfg->iommu_base == 0) {
            continue;  /* no IOMMU configured for this PCIe subsystem */
        }

        dev = qdev_new(TYPE_RISCV_IOMMU_SYS);
        qdev_prop_set_uint64(dev, "addr", pcfg->iommu_base);
        /* Connect IOMMU IRQs to root APLIC (the one with GPIO inputs) */
        if (s->aplic_root) {
            qdev_prop_set_uint32(dev, "base-irq", pcfg->iommu_base_irq);
            object_property_set_link(OBJECT(dev), "irqchip",
                                     OBJECT(s->aplic_root), &error_fatal);
        }
        DEVICE_GET_CLASS(dev)->vmsd = NULL;
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        iommu_devs[i] = dev;
    }

    /*
     * Pass 2: UNDO the auto-wired IOMMU → PCIe DMA interception.
     *
     * riscv_iommu_sys_realize() calls object_resolve_path_type() to find the
     * first TYPE_PCI_BUS and installs itself as the DMA address-space provider
     * via riscv_iommu_pci_setup_iommu().  In a multi-controller machine like
     * Beta SoC this auto-discovery may wire the wrong bus, but more critically
     * it must not wire ANY bus:
     *
     *   iommu-map is intentionally absent from all PCIe FDT nodes
     *   (Platform Guide §14.1).  The kernel therefore uses 1:1 physical
     *   addresses for DMA.  If QEMU routes those DMA transactions through the
     *   RISC-V IOMMU translation engine, every descriptor access faults
     *   (no translations populated) → virtio TX descriptors unreadable →
     *   TX never completes → NETDEV WATCHDOG fires every ~5 s.
     *
     * The IOMMU device remains fully realized and register-accessible so the
     * kernel IOMMU driver can probe it.  We simply clear bus->iommu_ops so
     * that pci_get_address_space() returns the DWC's direct system-memory
     * alias instead of the IOMMU IOVA space.
     */
    for (i = 0; i < cfg->num_pcie && i < 4; i++) {
        int j;

        if (!iommu_devs[i]) {
            continue;
        }

        for (j = 0; j < s->pcie_num_ctrl[i]; j++) {
            if (!s->pcie_host_devs[i][j]) {
                continue;
            }
            DesignwarePCIEHost *host =
                DESIGNWARE_PCIE_HOST(s->pcie_host_devs[i][j]);
            PCIBus *bus = PCI_HOST_BRIDGE(host)->bus;
            /* Remove IOMMU interception — restore direct DMA to system RAM. */
            bus->iommu_ops = NULL;
            bus->iommu_opaque = NULL;
        }
    }
}

/*
 * Reset handler for acpu_direct mode: sets a0=hartid, a1=fdt_base
 * after each machine reset (since cpu_reset clears all GPRs).
 */
static void beta_acpu_direct_reset(void *opaque)
{
    BetaMachineState *s = BETA_MACHINE(opaque);
    BetaSoCConfig *cfg = s->config;
    uint32_t num_acpu = cfg->acpu.num_clusters * cfg->acpu.cores_per_cluster;
    uint32_t i;

    for (i = 0; i < num_acpu; i++) {
        RISCVCPU *rcpu = RISCV_CPU(&s->acpu_soc.harts[i]);
        rcpu->env.gpr[10] = i;  /* a0 = hartid (ACPU 0-31) */
        if (s->fdt_load_addr) {
            rcpu->env.gpr[11] = s->fdt_load_addr;     /* a1 = FDT addr */
        }
    }

    /* Halt MCPU — not needed in acpu_direct mode */
    for (i = 0; i < cfg->mcpu.num_cores; i++) {
        CPUState *cs = CPU(&s->mcpu_soc.harts[i]);
        cs->halted = 1;
    }
}

/* ---------------------------------------------------------------------------
 * Step 14: Boot setup — place FDT in RAM and configure boot arguments
 * -------------------------------------------------------------------------*/

static void beta_boot_setup(BetaMachineState *s)
{
    BetaSoCConfig *cfg = s->config;
    BetaBootConfig *boot = &cfg->boot;

    /* Find a DDR region to place the FDT */
    hwaddr fdt_base = 0;
    hwaddr ddr_base = 0;
    uint32_t i;
    hwaddr ddr_size = 0;

    for (uint32_t j = 0; j < cfg->num_memory_regions; j++) {
        BetaMemRegion *mr = &cfg->memory_regions[j];
        if (g_strcmp0(mr->type, "ram") == 0 && mr->size >= 4 * MiB) {
            ddr_base = mr->base;
            ddr_size = mr->size;
            break;
        }
    }

    if (ddr_base == 0) {
        /* No suitable RAM found; use DDR channel 0 from config */
        if (cfg->ddr.num_channels > 0 && cfg->ddr.channels) {
            ddr_base = cfg->ddr.channels[0].mem_base;
            /*
             * Use only channel-0 size for FDT and kernel placement.
             * Channels are at non-contiguous physical addresses; summing all
             * channel sizes would place the FDT in a gap between channels.
             */
            ddr_size = cfg->ddr.channels[0].size;
        }
    }

    if (ddr_base) {
        /* Place FDT near the end of the first DDR region: base + size - 2MB */
        fdt_base = ddr_base + ddr_size - 2 * MiB;
        s->fdt_load_addr = fdt_base;
        if (s->soc_csr) {
            s->soc_csr->fdt_load_addr = fdt_base;
        }
    }

    /* Step 13: Build and load the FDT */
    if (MACHINE(s)->fdt == NULL) {
        beta_create_fdt(s);
    }

    /*
     * Support -kernel, -initrd, and -append in all boot modes.
     * Load kernel and initrd into DDR, add bootargs and initrd info
     * to the FDT chosen node.
     */
    {
        MachineState *ms = MACHINE(s);

        /* Load -kernel into DDR at fw_jump target (acpu_entry + 0x200000) */
        if (ms->kernel_filename && ddr_base) {
            hwaddr kernel_addr = boot->acpu_entry + 0x200000;
            if (kernel_addr < ddr_base ||
                kernel_addr >= ddr_base + ddr_size) {
                error_report("beta: kernel load addr 0x%" HWADDR_PRIx
                             " outside DDR [0x%" HWADDR_PRIx
                             "..0x%" HWADDR_PRIx ")",
                             kernel_addr, ddr_base, ddr_base + ddr_size);
                exit(1);
            }
            load_image_targphys(ms->kernel_filename, kernel_addr,
                                ddr_size - (kernel_addr - ddr_base));
        }

        /* Load -initrd and annotate FDT */
        if (ms->initrd_filename && ms->fdt && ddr_base) {
            /*
             * Place initrd after OpenSBI's FDT relocation area.
             * FW_JUMP relocates the FDT to FW_JUMP_ADDR + 32 MiB
             * (typically 0x4002200000).  Using 64 MiB offset avoids
             * any overlap with the relocated FDT.
             */
            hwaddr initrd_start = ddr_base + 64 * MiB;
            if (initrd_start >= ddr_base + ddr_size) {
                error_report("beta: initrd addr 0x%" HWADDR_PRIx
                             " outside DDR", initrd_start);
                exit(1);
            }
            ssize_t initrd_size = load_image_targphys(
                ms->initrd_filename, initrd_start,
                ddr_size - (initrd_start - ddr_base));
            if (initrd_size > 0) {
                qemu_fdt_setprop_u64(ms->fdt, "/chosen",
                                     "linux,initrd-start", initrd_start);
                qemu_fdt_setprop_u64(ms->fdt, "/chosen",
                                     "linux,initrd-end",
                                     initrd_start + initrd_size);
            }
        }

        /* Add -append as bootargs in FDT */
        if (ms->kernel_cmdline && ms->kernel_cmdline[0] && ms->fdt) {
            qemu_fdt_setprop_string(ms->fdt, "/chosen",
                                    "bootargs", ms->kernel_cmdline);
        }
    }

    if (MACHINE(s)->fdt && fdt_base) {
        riscv_load_fdt(fdt_base, MACHINE(s)->fdt);
    }

    /*
     * Step 14: Build and load MCPU FDT into MCPU SRAM.
     * Place at end of SRAM minus 64KB, similar to ACPU FDT in DDR.
     */
    {
        hwaddr mcpu_sram_base = 0, mcpu_sram_size = 0;
        uint32_t mi;
        for (mi = 0; mi < cfg->num_memory_regions; mi++) {
            if (g_strcmp0(cfg->memory_regions[mi].name, "mcpu_sram") == 0) {
                mcpu_sram_base = cfg->memory_regions[mi].base;
                mcpu_sram_size = cfg->memory_regions[mi].size;
                break;
            }
        }
        if (mcpu_sram_base && mcpu_sram_size >= 0x10000) {
            s->mcpu_fdt_addr = mcpu_sram_base + mcpu_sram_size - 0x10000;
            beta_create_mcpu_fdt(s);
            if (s->mcpu_fdt) {
                riscv_load_fdt(s->mcpu_fdt_addr, s->mcpu_fdt);
            }
        }
    }

    /*
     * For ACPU direct-boot mode (acpu_direct): skip MCPU and boot ACPU
     * directly from acpu_entry with FDT at fdt_base.  This is used for
     * standalone Linux testing without a real MCPU BootROM.
     *
     * Register a reset handler to set a0=hartid, a1=fdt_base after each
     * machine reset (cpu_reset clears all GPRs).  Also halt the MCPU.
     */
    if (g_strcmp0(boot->mode, "acpu_direct") == 0 && boot->acpu_entry) {
        qemu_register_reset(beta_acpu_direct_reset, s);
    }
}

/* ---------------------------------------------------------------------------
 * Machine init
 * -------------------------------------------------------------------------*/

static void beta_machine_init(MachineState *machine)
{
    BetaMachineState *s = BETA_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();


    /* Step 1: Load configuration from JSON file */
    if (!s->cfg_file) {
        error_report("Beta machine requires -M beta,config-file=<path.json>");
        exit(1);
    }
    s->config = beta_config_load(s->cfg_file, &error_fatal);

    beta_create_mcpu(s, system_memory);

    beta_create_acpu(s, system_memory);

    beta_create_memory_regions(s, system_memory);

    beta_load_firmware(s);

    beta_create_mcpu_plic(s, system_memory);
    beta_create_mcpu_ahb_window(s, system_memory);

    beta_create_clint(s);

    beta_create_aia(s);

    beta_create_uart(s, system_memory);
    beta_create_soc_csr(s, system_memory);
    beta_create_generic_mmio_devices(s, system_memory);
    beta_create_flash(s);
    beta_create_pcie(s, system_memory);
    /* Step 12c: IOMMU (one per PCIe subsystem) */
    beta_create_iommu(s);

    beta_boot_setup(s);
}

/* ---------------------------------------------------------------------------
 * QOM property: config-file
 * -------------------------------------------------------------------------*/

static char *beta_get_cfg_file(Object *obj, Error **errp)
{
    return g_strdup(BETA_MACHINE(obj)->cfg_file);
}

static void beta_set_cfg_file(Object *obj, const char *value, Error **errp)
{
    BetaMachineState *s = BETA_MACHINE(obj);
    g_free(s->cfg_file);
    s->cfg_file = g_strdup(value);
}

static bool beta_get_ztso(Object *obj, Error **errp)
{
    return BETA_MACHINE(obj)->ztso;
}

static void beta_set_ztso(Object *obj, bool value, Error **errp)
{
    BETA_MACHINE(obj)->ztso = value;
}

/* ---------------------------------------------------------------------------
 * Type registration
 * -------------------------------------------------------------------------*/

static void beta_machine_instance_init(Object *obj)
{
    BetaMachineState *s = BETA_MACHINE(obj);
    s->cfg_file       = NULL;
    s->ztso           = true;   /* Default: TSO (matches Beta hardware) */
    s->config         = NULL;
    s->aplic_m        = NULL;
    s->aplic_s        = NULL;
    s->clint          = NULL;
    s->flash          = NULL;
    s->fdt_load_addr  = 0;
    s->mcpu_fdt       = NULL;
    s->mcpu_fdt_addr  = 0;
    s->mcpu_plic      = NULL;
    s->mcpu_plic_irqs = NULL;
    s->mcpu_ahb       = NULL;
}

/*
 * Beta SoC ACPU ISA extensions — matches "Beta ISA Status.xlsx".
 * Beta supports full RVA23S64 mandatory profile + H extension + Ztso.
 *
 * Extensions already default true in QEMU base64 CPU (no need to list):
 *   zicsr, zifencei, zicntr, zihpm, zba, zbb, zbs,
 *   zicbom, zicbop, zicboz, zihintntl, zihintpause,
 *   zawrs, zfa, sstc, zic64b, sha, ssstateen
 */
/* Ztso is set directly in beta_create_acpu after realize */

static void beta_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc            = "Beta RISC-V SoC (configurable)";
    mc->init            = beta_machine_init;
    mc->max_cpus        = 33;   /* 32 ACPU + 1 MCPU */
    mc->default_cpu_type = TYPE_RISCV_CPU_BASE;
    mc->default_cpus    = 33;
    mc->min_cpus        = 1;
    mc->no_parallel     = true;
    mc->default_ram_id  = "beta.ram";
    /* No compat_props — Ztso set in beta_create_acpu post-realize */

    object_class_property_add_str(oc, "config-file",
                                  beta_get_cfg_file, beta_set_cfg_file);
    object_class_property_set_description(oc, "config-file",
        "Path to JSON hardware configuration file");

    object_class_property_add_bool(oc, "ztso",
                                   beta_get_ztso, beta_set_ztso);
    object_class_property_set_description(oc, "ztso",
        "Enable Ztso (TSO memory model). Default: true. "
        "Set to false for RVWMO mode to detect missing-fence bugs.");
}

static const TypeInfo beta_machine_typeinfo = {
    .name          = TYPE_BETA_MACHINE,
    .parent        = TYPE_MACHINE,
    .class_init    = beta_machine_class_init,
    .instance_init = beta_machine_instance_init,
    .instance_size = sizeof(BetaMachineState),
};

static void beta_machine_register_types(void)
{
    type_register_static(&beta_machine_typeinfo);
}
type_init(beta_machine_register_types)
