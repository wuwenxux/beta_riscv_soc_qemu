/*
 * Beta SoC - Device Tree generation from JSON config
 *
 * Generates a minimal but complete FDT for Linux boot:
 *   - /cpus (ACPU harts only; MCPU is invisible to Linux)
 *   - /memory (DDR regions)
 *   - /soc: ACLINT MSWI, ACLINT MTIMER, IMSIC M/S, APLIC M/S, UART
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/riscv/beta.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/intc/riscv_aplic.h"
#include "hw/intc/riscv_imsic.h"
#include "hw/riscv/iommu.h"
#include "system/device_tree.h"
#include <libfdt.h>

#include "beta_config.h"

/* RISC-V interrupt numbers (cpu-intc perspective) */
#define IRQ_M_SOFT   3
#define IRQ_M_TIMER  7
#define IRQ_M_EXT   11
#define IRQ_S_SOFT   1
#define IRQ_S_TIMER  5
#define IRQ_S_EXT    9

/* AIA FDT cell counts */
#define FDT_IMSIC_INT_CELLS   0
#define FDT_APLIC_ADDR_CELLS  0
#define FDT_APLIC_INT_CELLS   2

/* ceil(log2(count)) */
static uint32_t dtb_num_bits(uint32_t count)
{
    uint32_t ret = 0;
    while ((1U << ret) < count) {
        ret++;
    }
    return ret;
}

/* ---------------------------------------------------------------------------
 * /cpus node: per-hart cpu@N nodes, cache topology, and cluster cpu-map.
 *
 * Each ACPU hart gets an "intc" interrupt-controller sub-node (phandle),
 * L1 i-cache / d-cache properties, and a next-level-cache link to its
 * cluster's L2 node.
 *
 * Cache hierarchy (under /cpus):
 *   l3-cache          — single node shared by all cores
 *   l2-cache0..N      — one per cluster, shared by cores_per_cluster cores
 *   cpu@N properties  — L1 i/d cache sizes, sets, block-size
 *
 * Cluster topology (under /cpus/cpu-map):
 *   cluster0/core0..3 -> cpu@0..3   (each core node has "cpu" phandle)
 *   cluster1/core0..3 -> cpu@4..7
 *   ...
 *
 * Cache nodes are only emitted when the JSON config provides non-zero
 * cache parameters (acpu.cache).
 *
 * Returns array of per-hart intc phandles (caller must g_free).
 * -------------------------------------------------------------------------*/

static uint32_t *beta_fdt_create_cpus(BetaMachineState *s,
                                      uint32_t *phandle)
{
    MachineState *ms = MACHINE(s);
    BetaACPUConfig *acpu = &s->config->acpu;
    BetaCLINTConfig *clint = &s->config->interrupts.clint;
    BetaCacheConfig *cache = &acpu->cache;
    uint32_t num_clusters = acpu->num_clusters;
    uint32_t cores_per_cluster = acpu->cores_per_cluster;
    uint32_t num_acpu = num_clusters * cores_per_cluster;
    uint32_t acpu_hartid_base = 0;  /* ACPU hartid 0-31 */
    uint32_t timebase_freq = clint->timebase_freq ?
                             clint->timebase_freq :
                             RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ;
    uint32_t *intc_phandles = g_new0(uint32_t, num_acpu);
    uint32_t *cpu_phandles = g_new0(uint32_t, num_acpu);
    uint32_t i, cluster, core;

    /* Whether cache topology is present in the config */
    bool has_cache = cache->l1i_size_kb && cache->l1i_ways &&
                     cache->l1d_size_kb && cache->l1d_ways &&
                     cache->line_bytes;
    bool has_l2 = has_cache && cache->l2_size_kb && cache->l2_ways;
    bool has_l3 = has_cache && cache->l3_slice_kb && cache->l3_num_slices;

    /* Pre-computed cache geometry */
    uint32_t l1i_size = 0, l1i_sets = 0;
    uint32_t l1d_size = 0, l1d_sets = 0;
    uint32_t l2_size = 0, l2_sets = 0;
    uint32_t l3_size = 0, l3_sets = 0;
    uint32_t line = 0;

    if (has_cache) {
        line = cache->line_bytes;
        l1i_size = cache->l1i_size_kb * 1024;
        l1i_sets = l1i_size / (cache->l1i_ways * line);
        l1d_size = cache->l1d_size_kb * 1024;
        l1d_sets = l1d_size / (cache->l1d_ways * line);
    }
    if (has_l2) {
        l2_size = cache->l2_size_kb * 1024;
        l2_sets = l2_size / (cache->l2_ways * line);
    }
    if (has_l3) {
        l3_size = cache->l3_slice_kb * 1024 * cache->l3_num_slices;
        /* 16-way associativity from doc */
        l3_sets = l3_size / (16 * line);
    }

    /* Phandle arrays for cache nodes (allocated if needed) */
    uint32_t l3_phandle = 0;
    uint32_t *l2_phandles = NULL;

    if (has_l3) {
        l3_phandle = (*phandle)++;
    }
    if (has_l2) {
        l2_phandles = g_new0(uint32_t, num_clusters);
        for (i = 0; i < num_clusters; i++) {
            l2_phandles[i] = (*phandle)++;
        }
    }

    qemu_fdt_add_subnode(ms->fdt, "/cpus");
    qemu_fdt_setprop_cell(ms->fdt, "/cpus", "#address-cells", 1);
    qemu_fdt_setprop_cell(ms->fdt, "/cpus", "#size-cells", 0);
    qemu_fdt_setprop_cell(ms->fdt, "/cpus", "timebase-frequency",
                          timebase_freq);

    /* --- L3 cache node (child of /cpus) --- */
    if (has_l3) {
        g_autofree char *l3_name = g_strdup_printf("/cpus/l3-cache");

        qemu_fdt_add_subnode(ms->fdt, l3_name);
        qemu_fdt_setprop_string(ms->fdt, l3_name, "compatible", "cache");
        qemu_fdt_setprop(ms->fdt, l3_name, "cache-unified", NULL, 0);
        qemu_fdt_setprop_cell(ms->fdt, l3_name, "cache-size", l3_size);
        qemu_fdt_setprop_cell(ms->fdt, l3_name, "cache-sets", l3_sets);
        qemu_fdt_setprop_cell(ms->fdt, l3_name, "cache-block-size", line);
        qemu_fdt_setprop_cell(ms->fdt, l3_name, "cache-level", 3);
        qemu_fdt_setprop_cell(ms->fdt, l3_name, "phandle", l3_phandle);
    }

    /* --- Per-cluster L2 cache nodes (children of /cpus) --- */
    if (has_l2) {
        for (i = 0; i < num_clusters; i++) {
            g_autofree char *l2_name =
                g_strdup_printf("/cpus/l2-cache%u", i);

            qemu_fdt_add_subnode(ms->fdt, l2_name);
            qemu_fdt_setprop_string(ms->fdt, l2_name, "compatible", "cache");
            qemu_fdt_setprop(ms->fdt, l2_name, "cache-unified", NULL, 0);
            qemu_fdt_setprop_cell(ms->fdt, l2_name, "cache-size", l2_size);
            qemu_fdt_setprop_cell(ms->fdt, l2_name, "cache-sets", l2_sets);
            qemu_fdt_setprop_cell(ms->fdt, l2_name, "cache-block-size", line);
            qemu_fdt_setprop_cell(ms->fdt, l2_name, "cache-level", 2);
            if (has_l3) {
                qemu_fdt_setprop_cell(ms->fdt, l2_name, "next-level-cache",
                                      l3_phandle);
            }
            qemu_fdt_setprop_cell(ms->fdt, l2_name, "phandle",
                                  l2_phandles[i]);
        }
    }

    /* --- Per-hart cpu@N nodes --- */
    /*
     * qemu_fdt_add_subnode() prepends each node, so iterating in reverse
     * order ensures cpu@0 is added last and therefore appears first in the
     * DTB.  This keeps interrupts-extended[i] aligned with hartid i.
     */
    for (i = num_acpu; i-- > 0; ) {
        uint32_t hartid = acpu_hartid_base + i;
        uint32_t cpu_ph = (*phandle)++;
        uint32_t intc_phandle = (*phandle)++;
        uint32_t this_cluster = i / cores_per_cluster;
        g_autofree char *cpu_name = g_strdup_printf("/cpus/cpu@%u", hartid);
        g_autofree char *intc_name = g_strdup_printf("/cpus/cpu@%u/interrupt-controller",
                                                      hartid);
        const char *isa = acpu->isa ? acpu->isa : "rv64imafdc";
        const char *mmu = acpu->mmu_mode ? acpu->mmu_mode : "sv39";
        g_autofree char *mmu_type = g_strdup_printf("riscv,%s", mmu);

        qemu_fdt_add_subnode(ms->fdt, cpu_name);
        qemu_fdt_setprop_string(ms->fdt, cpu_name, "device_type", "cpu");
        qemu_fdt_setprop_cell(ms->fdt, cpu_name, "reg", hartid);
        qemu_fdt_setprop_string(ms->fdt, cpu_name, "compatible", "riscv");
        qemu_fdt_setprop_string(ms->fdt, cpu_name, "status", "okay");
        qemu_fdt_setprop_string(ms->fdt, cpu_name, "riscv,isa", isa);
        qemu_fdt_setprop_string(ms->fdt, cpu_name, "riscv,isa-base", "rv64i");
        qemu_fdt_setprop_string(ms->fdt, cpu_name, "mmu-type", mmu_type);
        qemu_fdt_setprop_cell(ms->fdt, cpu_name, "phandle", cpu_ph);

        /* NUMA node id: each cluster maps to one NUMA domain */
        if (num_clusters > 1) {
            qemu_fdt_setprop_cell(ms->fdt, cpu_name, "numa-node-id",
                                  this_cluster);
        }

        /*
         * ISA extensions — complete Beta hardware support list.
         * Aligned with "Beta ISA Status.xlsx" (all Supported items).
         */
        {
            static const char * const isa_exts[] = {
                /* Base ISA + MISA */
                "i", "m", "a", "f", "d", "c", "v", "h", "b",
                /* Privileged */
                "zicsr", "zifencei", "zicntr", "zihpm",
                /* AIA + Timer */
                "smaia", "ssaia", "sstc",
                /* Bit manipulation */
                "zba", "zbb", "zbs",
                /* Scalar crypto: only Zkt (timing-safe) per ISA Status */
                "zkt",
                /* Half-precision float */
                "zfhmin", "zfa",
                /* Cache block ops */
                "zicbom", "zicbop", "zicboz",
                /* Hints + Atomics */
                "zihintpause", "zihintntl", "zawrs",
                /* Vector sub-extensions */
                "zvfhmin", "zvbb", "zvkt",
                /* Vector length: VLEN=512 (implies zvl64b, zvl128b, zvl256b) */
                "zvl64b", "zvl128b", "zvl256b", "zvl512b",
                /* Compressed */
                "zcb", "zcmop",
                /* Integer conditional + may-be-ops */
                "zicond", "zimop",
                /* Supervisor extensions */
                "svnapot", "svpbmt", "svinval", "svade",
                "sscofpmf", "ssnpm", "supm",
                /* Named features (cache/memory model) */
                "zic64b", "ziccif", "ziccrse", "ziccamoa", "zicclsm",
                "za64rs",
                /* Privilege named features */
                /* ss1p13: Beta-internal tag for Supervisor Specification v1.13;
                 * not a ratified RISC-V ISA extension — Linux silently ignores it */
                "ss1p13", "svbare", "sv39",
                "ssccptr", "sstvecd", "sstvala", "sscounterenw", "ssu64xl",
                /* Hypervisor named features */
                "ssstateen", "shcounterenw",
                "shvstvala", "shtvala", "shvstvecd", "shvsatpa", "shgatpa",
                /* Beta-specific */
                "ztso",
            };
            int num_exts = ARRAY_SIZE(isa_exts);
            qemu_fdt_setprop_string_array(ms->fdt, cpu_name,
                                          "riscv,isa-extensions",
                                          (char **)isa_exts, num_exts);
        }

        /* Cache block sizes (required by Zicbom/Zicboz FDT binding) */
        qemu_fdt_setprop_cell(ms->fdt, cpu_name, "riscv,cbom-block-size", 64);
        qemu_fdt_setprop_cell(ms->fdt, cpu_name, "riscv,cboz-block-size", 64);

        /* L1 cache properties on the cpu node */
        if (has_cache) {
            qemu_fdt_setprop_cell(ms->fdt, cpu_name,
                                  "i-cache-size", l1i_size);
            qemu_fdt_setprop_cell(ms->fdt, cpu_name,
                                  "i-cache-sets", l1i_sets);
            qemu_fdt_setprop_cell(ms->fdt, cpu_name,
                                  "i-cache-block-size", line);
            qemu_fdt_setprop_cell(ms->fdt, cpu_name,
                                  "d-cache-size", l1d_size);
            qemu_fdt_setprop_cell(ms->fdt, cpu_name,
                                  "d-cache-sets", l1d_sets);
            qemu_fdt_setprop_cell(ms->fdt, cpu_name,
                                  "d-cache-block-size", line);
            if (has_l2) {
                qemu_fdt_setprop_cell(ms->fdt, cpu_name,
                                      "next-level-cache",
                                      l2_phandles[this_cluster]);
            }
        }

        /* interrupt-controller sub-node (cpu-local intc) */
        qemu_fdt_add_subnode(ms->fdt, intc_name);
        qemu_fdt_setprop_string(ms->fdt, intc_name, "compatible",
                                "riscv,cpu-intc");
        qemu_fdt_setprop(ms->fdt, intc_name, "interrupt-controller",
                         NULL, 0);
        qemu_fdt_setprop_cell(ms->fdt, intc_name, "#interrupt-cells", 1);
        qemu_fdt_setprop_cell(ms->fdt, intc_name, "phandle", intc_phandle);

        cpu_phandles[i] = cpu_ph;
        intc_phandles[i] = intc_phandle;
    }

    /* --- /cpus/cpu-map: cluster topology (8 clusters × 4 cores) --- */
    qemu_fdt_add_subnode(ms->fdt, "/cpus/cpu-map");

    for (cluster = 0; cluster < num_clusters; cluster++) {
        g_autofree char *clust_name =
            g_strdup_printf("/cpus/cpu-map/cluster%u", cluster);
        qemu_fdt_add_subnode(ms->fdt, clust_name);

        for (core = 0; core < cores_per_cluster; core++) {
            uint32_t cpu_idx = cluster * cores_per_cluster + core;
            g_autofree char *core_name =
                g_strdup_printf("%s/core%u", clust_name, core);
            qemu_fdt_add_subnode(ms->fdt, core_name);
            qemu_fdt_setprop_cell(ms->fdt, core_name, "cpu",
                                  cpu_phandles[cpu_idx]);
        }
    }

    g_free(cpu_phandles);
    g_free(l2_phandles);

    return intc_phandles;
}

/* ---------------------------------------------------------------------------
 * /memory nodes (DDR)
 * -------------------------------------------------------------------------*/

static void beta_fdt_create_memory(BetaMachineState *s)
{
    MachineState *ms = MACHINE(s);
    BetaSoCConfig *cfg = s->config;
    uint32_t num_clusters = cfg->acpu.num_clusters;
    uint32_t i;

    if (cfg->ddr.num_channels == 0 || !cfg->ddr.channels[0].mem_base) {
        return;
    }

    /*
     * NUMA-aware memory layout (num_clusters > 1):
     *   Create one /memory node per DDR channel, each tagged with
     *   numa-node-id matching the cluster that is "closest" to that
     *   channel.  Channel i maps to NUMA node i (cluster i).
     *
     * Non-NUMA fallback:
     *   Report DDR as one contiguous region (original behaviour).
     *   Beta 8 DDR channels are interleaved at 64-byte granularity,
     *   so CPU sees contiguous memory starting at channel 0 base.
     */
    if (num_clusters > 1) {
        for (i = 0; i < cfg->ddr.num_channels; i++) {
            BetaDDRChannel *ch = &cfg->ddr.channels[i];
            if (ch->size == 0) {
                continue;
            }
            g_autofree char *name =
                g_strdup_printf("/memory@%" PRIx64, ch->mem_base);
            qemu_fdt_add_subnode(ms->fdt, name);
            qemu_fdt_setprop_string(ms->fdt, name, "device_type", "memory");
            qemu_fdt_setprop_cells(ms->fdt, name, "reg",
                                   (uint32_t)(ch->mem_base >> 32),
                                   (uint32_t)ch->mem_base,
                                   (uint32_t)(ch->size >> 32),
                                   (uint32_t)ch->size);
            /* NUMA: channel i is local to cluster i */
            qemu_fdt_setprop_cell(ms->fdt, name, "numa-node-id",
                                  i % num_clusters);
        }
    } else {
        hwaddr ddr_base = cfg->ddr.channels[0].mem_base;
        hwaddr total_size = 0;
        for (i = 0; i < cfg->ddr.num_channels; i++) {
            total_size += cfg->ddr.channels[i].size;
        }
        if (total_size > 0) {
            g_autofree char *name = g_strdup_printf("/memory@%" PRIx64,
                                                    ddr_base);
            qemu_fdt_add_subnode(ms->fdt, name);
            qemu_fdt_setprop_string(ms->fdt, name, "device_type", "memory");
            qemu_fdt_setprop_cells(ms->fdt, name, "reg",
                                   (uint32_t)(ddr_base >> 32),
                                   (uint32_t)ddr_base,
                                   (uint32_t)(total_size >> 32),
                                   (uint32_t)total_size);
        }
    }
}

/* ---------------------------------------------------------------------------
 * /distance-map — NUMA distance matrix for inter-cluster topology
 *
 * Beta SoC NOC is a 4x2 mesh (4 columns, 2 rows).
 * Cluster coordinates:
 *   Row 0: cluster 0(0,0) 1(1,0) 2(2,0) 3(3,0)
 *   Row 1: cluster 4(0,1) 5(1,1) 6(2,1) 7(3,1)
 *
 * Distance model:
 *   Self (same cluster):          10
 *   Same row (|y_diff| == 0):     20
 *   Different row (|y_diff| == 1): 30
 * -------------------------------------------------------------------------*/

static void beta_fdt_create_numa_distance_map(BetaMachineState *s)
{
    MachineState *ms = MACHINE(s);
    uint32_t num_clusters = s->config->acpu.num_clusters;
    uint32_t i, j;
    uint32_t *matrix;
    uint32_t entries;

    if (num_clusters <= 1) {
        return;
    }

    /*
     * distance-matrix property: array of <node_from node_to distance>
     * triples for every ordered (i, j) pair.
     */
    entries = num_clusters * num_clusters;
    matrix = g_new0(uint32_t, entries * 3);

    for (i = 0; i < num_clusters; i++) {
        uint32_t row_i = i / 4;  /* 4 clusters per row */
        for (j = 0; j < num_clusters; j++) {
            uint32_t row_j = j / 4;
            uint32_t dist;
            uint32_t idx = (i * num_clusters + j) * 3;

            if (i == j) {
                dist = 10;  /* local access */
            } else if (row_i == row_j) {
                dist = 20;  /* same row, different column */
            } else {
                dist = 30;  /* cross-row */
            }

            matrix[idx + 0] = cpu_to_be32(i);
            matrix[idx + 1] = cpu_to_be32(j);
            matrix[idx + 2] = cpu_to_be32(dist);
        }
    }

    qemu_fdt_add_subnode(ms->fdt, "/distance-map");
    qemu_fdt_setprop_string(ms->fdt, "/distance-map", "compatible",
                            "numa-distance-map-v1");
    qemu_fdt_setprop(ms->fdt, "/distance-map", "distance-matrix",
                     matrix, entries * 3 * sizeof(uint32_t));

    g_free(matrix);
}

/* ---------------------------------------------------------------------------
 * /soc/mswi@N  — ACLINT machine-mode software interrupts
 * -------------------------------------------------------------------------*/

static void beta_fdt_create_mswi(BetaMachineState *s,
                                 const uint32_t *intc_phandles,
                                 uint32_t num_acpu,
                                 uint32_t acpu_hartid_base)
{
    MachineState *ms = MACHINE(s);
    BetaCLINTConfig *clint = &s->config->interrupts.clint;
    hwaddr base = clint->base;
    hwaddr size = RISCV_ACLINT_SWI_SIZE;
    g_autofree uint32_t *cells = g_new0(uint32_t, num_acpu * 2);
    g_autofree char *name = NULL;
    uint32_t i;

    if (!base) {
        return;
    }

    name = g_strdup_printf("/soc/mswi@%" PRIx64, base);

    for (i = 0; i < num_acpu; i++) {
        cells[i * 2 + 0] = cpu_to_be32(intc_phandles[i]);
        cells[i * 2 + 1] = cpu_to_be32(IRQ_M_SOFT);
    }

    qemu_fdt_add_subnode(ms->fdt, name);
    qemu_fdt_setprop_string(ms->fdt, name, "compatible",
                            "riscv,aclint-mswi");
    qemu_fdt_setprop_cells(ms->fdt, name, "reg",
                           (uint32_t)(base >> 32), (uint32_t)base,
                           (uint32_t)(size >> 32), (uint32_t)size);
    qemu_fdt_setprop(ms->fdt, name, "interrupts-extended",
                     cells, num_acpu * sizeof(uint32_t) * 2);
}

/* ---------------------------------------------------------------------------
 * /soc/mtimer@N  — ACLINT machine-mode timer
 * -------------------------------------------------------------------------*/

static void beta_fdt_create_mtimer(BetaMachineState *s,
                                   const uint32_t *intc_phandles,
                                   uint32_t num_acpu,
                                   uint32_t acpu_hartid_base)
{
    MachineState *ms = MACHINE(s);
    BetaCLINTConfig *clint = &s->config->interrupts.clint;
    hwaddr base = clint->base + RISCV_ACLINT_SWI_SIZE;
    hwaddr size = RISCV_ACLINT_DEFAULT_MTIMER_SIZE;
    uint32_t freq = clint->timebase_freq ? clint->timebase_freq
                                         : RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ;
    g_autofree uint32_t *cells = g_new0(uint32_t, num_acpu * 2);
    g_autofree char *name = NULL;
    uint32_t i;

    if (!clint->base) {
        return;
    }

    name = g_strdup_printf("/soc/mtimer@%" PRIx64, base);

    for (i = 0; i < num_acpu; i++) {
        cells[i * 2 + 0] = cpu_to_be32(intc_phandles[i]);
        cells[i * 2 + 1] = cpu_to_be32(IRQ_M_TIMER);
    }

    qemu_fdt_add_subnode(ms->fdt, name);
    qemu_fdt_setprop_string(ms->fdt, name, "compatible",
                            "riscv,aclint-mtimer");
    /*
     * ACLINT mtimer reg: two entries per the DT binding:
     *   1. mtime register:  base + 0x7ff8, size 8
     *   2. mtimecmp array:  base + 0x0000, size 0x7ff8
     */
    {
        hwaddr mtime_addr = base + RISCV_ACLINT_DEFAULT_MTIME;
        hwaddr mtimecmp_addr = base + RISCV_ACLINT_DEFAULT_MTIMECMP;
        qemu_fdt_setprop_cells(ms->fdt, name, "reg",
                               (uint32_t)(mtime_addr >> 32), (uint32_t)mtime_addr,
                               0, (uint32_t)(size - RISCV_ACLINT_DEFAULT_MTIME),
                               (uint32_t)(mtimecmp_addr >> 32), (uint32_t)mtimecmp_addr,
                               0, (uint32_t)RISCV_ACLINT_DEFAULT_MTIME);
    }
    qemu_fdt_setprop(ms->fdt, name, "interrupts-extended",
                     cells, num_acpu * sizeof(uint32_t) * 2);
    qemu_fdt_setprop_cell(ms->fdt, name, "riscv,timebase-frequency", freq);
}

/* ---------------------------------------------------------------------------
 * /soc/interrupt-controller@N  — IMSIC (M-mode or S-mode)
 * -------------------------------------------------------------------------*/

static void beta_fdt_create_imsic(BetaMachineState *s,
                                  const uint32_t *intc_phandles,
                                  uint32_t num_acpu,
                                  bool mmode,
                                  uint32_t guest_bits,
                                  uint32_t phandle)
{
    MachineState *ms = MACHINE(s);
    BetaIMSICConfig *icfg = &s->config->interrupts.imsic;
    hwaddr base = mmode ? icfg->m_base : icfg->s_base;
    uint32_t hart_size = IMSIC_HART_SIZE(mmode ? 0 : guest_bits);
    uint32_t num_clusters = s->config->acpu.num_clusters;
    uint32_t cores_per_cluster = s->config->acpu.cores_per_cluster;
    hwaddr group_stride = (hwaddr)1 << IMSIC_MMIO_GROUP_MIN_SHIFT;
    int irq_ext = mmode ? IRQ_M_EXT : IRQ_S_EXT;
    uint32_t num_ids = icfg->num_ids ? icfg->num_ids : 255;
    g_autofree uint32_t *cells = g_new0(uint32_t, num_acpu * 2);
    /* 4 uint32 cells per cluster (hi_addr, lo_addr, hi_size, lo_size) */
    g_autofree uint32_t *regs = g_new0(uint32_t, num_clusters * 4);
    g_autofree char *name = NULL;
    uint32_t i;
    static const char * const compat[2] = { "riscv,imsics", NULL };

    if (!base) {
        return;
    }

    name = g_strdup_printf("/soc/interrupt-controller@%" PRIx64, base);

    for (i = 0; i < num_acpu; i++) {
        cells[i * 2 + 0] = cpu_to_be32(intc_phandles[i]);
        cells[i * 2 + 1] = cpu_to_be32(irq_ext);
    }

    /* Per-cluster reg entries: each cluster has its own IMSIC address range */
    for (i = 0; i < num_clusters; i++) {
        hwaddr cluster_base = base + (hwaddr)i * group_stride;
        hwaddr cluster_size = (hwaddr)cores_per_cluster * hart_size;
        regs[i * 4 + 0] = cpu_to_be32((uint32_t)(cluster_base >> 32));
        regs[i * 4 + 1] = cpu_to_be32((uint32_t)cluster_base);
        regs[i * 4 + 2] = cpu_to_be32((uint32_t)(cluster_size >> 32));
        regs[i * 4 + 3] = cpu_to_be32((uint32_t)cluster_size);
    }

    qemu_fdt_add_subnode(ms->fdt, name);
    qemu_fdt_setprop_string(ms->fdt, name, "compatible", compat[0]);
    qemu_fdt_setprop_cell(ms->fdt, name, "#interrupt-cells",
                          FDT_IMSIC_INT_CELLS);
    qemu_fdt_setprop(ms->fdt, name, "interrupt-controller", NULL, 0);
    qemu_fdt_setprop(ms->fdt, name, "msi-controller", NULL, 0);
    qemu_fdt_setprop_cell(ms->fdt, name, "#msi-cells", 0);
    qemu_fdt_setprop(ms->fdt, name, "interrupts-extended",
                     cells, num_acpu * sizeof(uint32_t) * 2);
    qemu_fdt_setprop(ms->fdt, name, "reg",
                     regs, num_clusters * sizeof(uint32_t) * 4);
    qemu_fdt_setprop_cell(ms->fdt, name, "riscv,num-ids", num_ids);

    if (!mmode && guest_bits > 0) {
        qemu_fdt_setprop_cell(ms->fdt, name, "riscv,guest-index-bits",
                              guest_bits);
    }

    /* Per-cluster IMSIC grouping: hart_index_bits + group_index_bits/shift */
    qemu_fdt_setprop_cell(ms->fdt, name, "riscv,hart-index-bits",
                          dtb_num_bits(cores_per_cluster));
    qemu_fdt_setprop_cell(ms->fdt, name, "riscv,group-index-bits",
                          dtb_num_bits(num_clusters));
    qemu_fdt_setprop_cell(ms->fdt, name, "riscv,group-index-shift",
                          IMSIC_MMIO_GROUP_MIN_SHIFT);

    qemu_fdt_setprop_cell(ms->fdt, name, "phandle", phandle);
}

/* ---------------------------------------------------------------------------
 * /soc/interrupt-controller@N  — APLIC domain (generic helper)
 *
 * Beta 3-level APLIC:
 *   MLROOT (direct delivery → MCPU) → MLAPP (MSI → IMSIC M) → SLAPP (MSI → IMSIC S)
 * -------------------------------------------------------------------------*/

static uint32_t beta_fdt_create_aplic_domain(
    BetaMachineState *s,
    const char *level,          /* "root", "m", or "s" */
    uint32_t phandle,
    uint32_t msi_phandle,       /* 0 for direct delivery (root) */
    uint32_t child_phandle,     /* 0 if leaf */
    uint32_t *intc_phandles,    /* for direct delivery: hart intc phandles */
    uint32_t num_direct_harts,  /* for direct delivery: number of harts */
    uint32_t direct_hartid_base)/* for direct delivery: base hartid */
{
    MachineState *ms = MACHINE(s);
    BetaAPLICConfig *acfg = &s->config->interrupts.aplic;
    uint32_t num_sources = acfg->num_sources ? acfg->num_sources : 1023;
    BetaAPLICDomain *domain = NULL;
    uint32_t i;

    for (i = 0; i < acfg->num_domains; i++) {
        if (g_strcmp0(acfg->domains[i].level, level) == 0) {
            domain = &acfg->domains[i];
            break;
        }
    }
    if (!domain || !domain->base) {
        return 0;
    }

    hwaddr base = domain->base;
    hwaddr size = domain->size ? domain->size : 0x10000;
    g_autofree char *name = g_strdup_printf(
        "/soc/interrupt-controller@%" PRIx64, base);

    qemu_fdt_add_subnode(ms->fdt, name);
    qemu_fdt_setprop_string(ms->fdt, name, "compatible", "riscv,aplic");
    qemu_fdt_setprop_cell(ms->fdt, name, "#address-cells",
                          FDT_APLIC_ADDR_CELLS);
    qemu_fdt_setprop_cell(ms->fdt, name, "#interrupt-cells",
                          FDT_APLIC_INT_CELLS);
    qemu_fdt_setprop(ms->fdt, name, "interrupt-controller", NULL, 0);

    qemu_fdt_setprop_cells(ms->fdt, name, "reg",
                           (uint32_t)(base >> 32), (uint32_t)base,
                           (uint32_t)(size >> 32), (uint32_t)size);
    qemu_fdt_setprop_cell(ms->fdt, name, "riscv,num-sources", num_sources);

    if (msi_phandle) {
        /* MSI mode: deliver to IMSIC */
        qemu_fdt_setprop_cell(ms->fdt, name, "msi-parent", msi_phandle);
    } else if (intc_phandles && num_direct_harts > 0) {
        /* Direct delivery mode: wire to hart interrupt controllers */
        uint32_t *cells = g_new0(uint32_t, num_direct_harts * 2);
        for (i = 0; i < num_direct_harts; i++) {
            cells[i * 2]     = cpu_to_be32(intc_phandles[direct_hartid_base + i]);
            cells[i * 2 + 1] = cpu_to_be32(0xB); /* M-mode external interrupt */
        }
        qemu_fdt_setprop(ms->fdt, name, "interrupts-extended",
                         cells, num_direct_harts * sizeof(uint32_t) * 2);
        g_free(cells);
    }

    if (child_phandle) {
        qemu_fdt_setprop_cell(ms->fdt, name, "riscv,children",
                              child_phandle);
        qemu_fdt_setprop_cells(ms->fdt, name, "riscv,delegation",
                               child_phandle, 1, num_sources);
        qemu_fdt_setprop_cells(ms->fdt, name, "riscv,delegate",
                               child_phandle, 1, num_sources);
    }

    qemu_fdt_setprop_cell(ms->fdt, name, "phandle", phandle);
    return phandle;
}

/* ---------------------------------------------------------------------------
 * /soc/serial@N  — UART (ns16550a)
 * -------------------------------------------------------------------------*/

static void beta_fdt_create_uart(BetaMachineState *s,
                                 uint32_t aplic_s_phandle)
{
    MachineState *ms = MACHINE(s);
    BetaSoCConfig *cfg = s->config;
    BetaDeviceConfig *uart = NULL;
    uint32_t i;

    for (i = 0; i < cfg->num_devices; i++) {
        if (g_strcmp0(cfg->devices[i].type, "uart") == 0) {
            uart = &cfg->devices[i];
            break;
        }
    }

    if (!uart || !uart->base) {
        return;
    }

    g_autofree char *name = g_strdup_printf("/soc/serial@%" PRIx64,
                                            uart->base);
    hwaddr size = uart->size ? uart->size : 0x100;
    uint32_t clock_hz = uart->clock_hz ? uart->clock_hz : 3686400;
    int irq_num = 0;

    if (uart->irq_name) {
        irq_num = 0;
        for (i = 0; i < cfg->interrupts.num_irqs; i++) {
            if (g_strcmp0(cfg->interrupts.irq_map[i].name,
                          uart->irq_name) == 0) {
                irq_num = (int)cfg->interrupts.irq_map[i].irq_num;
                break;
            }
        }
    }

    qemu_fdt_add_subnode(ms->fdt, name);
    qemu_fdt_setprop_string(ms->fdt, name, "compatible", "ns16550a");
    qemu_fdt_setprop_cells(ms->fdt, name, "reg",
                           (uint32_t)(uart->base >> 32),
                           (uint32_t)uart->base,
                           (uint32_t)(size >> 32),
                           (uint32_t)size);
    qemu_fdt_setprop_cell(ms->fdt, name, "clock-frequency", clock_hz);
    qemu_fdt_setprop_cell(ms->fdt, name, "reg-shift", 2);
    qemu_fdt_setprop_cell(ms->fdt, name, "reg-io-width", 1);

    if (irq_num > 0 && aplic_s_phandle) {
        qemu_fdt_setprop_cell(ms->fdt, name, "interrupt-parent",
                              aplic_s_phandle);
        qemu_fdt_setprop_cells(ms->fdt, name, "interrupts",
                               irq_num, 0x4 /* IRQ_TYPE_LEVEL_HIGH */);
    }

    /* Include baud rate in stdout-path so DT earlycon configures UART correctly */
    {
        g_autofree char *stdout_path = g_strdup_printf("%s:115200n8", name);
        qemu_fdt_setprop_string(ms->fdt, "/chosen", "stdout-path", stdout_path);
    }
    qemu_fdt_setprop_string(ms->fdt, "/aliases", "serial0", name);
}

/* ---------------------------------------------------------------------------
 * /soc/iommu@N  — RISC-V IOMMU (one per PCIe subsystem)
 *
 * Beta has one IOMMU per PCIe controller, located in the AXI bridge
 * CSR space at offset 0x1800 from each bridge base.
 * AXI bridge bases: 0x07_0000_0000 + i * 0x2000
 *
 * Each IOMMU has 4 wired IRQs starting at APLIC source (100 + i*4).
 * -------------------------------------------------------------------------*/

static void beta_fdt_create_iommu(BetaMachineState *s,
                                  uint32_t aplic_s_phandle,
                                  uint32_t *iommu_phandles,
                                  uint32_t *phandle)
{
    MachineState *ms = MACHINE(s);
    BetaSoCConfig *cfg = s->config;
    uint32_t p;

    static const hwaddr axi_bridge_bases[] = {
        0x0700000000ULL, 0x0700002000ULL,
        0x0700004000ULL, 0x0700006000ULL,
    };

    /*
     * Only create IOMMU FDT nodes for production config (40-bit addresses).
     * Test config uses low 32-bit addresses where AXI bridges don't exist.
     */
    if (cfg->num_pcie == 0 || cfg->pcie[0].dbi_base < 0x100000000ULL) {
        return;
    }

    for (p = 0; p < cfg->num_pcie && p < 4; p++) {
        BetaPCIeConfig *pcfg = &cfg->pcie[p];
        hwaddr iommu_addr;
        hwaddr iommu_size = 0x1000;  /* RISCV_IOMMU_REG_SIZE */
        uint32_t base_irq;
        uint32_t iommu_ph;

        if (pcfg->dbi_base == 0) {
            continue;
        }

        /* IOMMU registers at AXI bridge base + 0x1800 */
        iommu_addr = axi_bridge_bases[p] + 0x1800;
        /* Per interrupt vector table rev2: one IRQ per IOMMU at 44/45/46/47 */
        base_irq = 44 + p;
        iommu_ph = (*phandle)++;

        g_autofree char *name = g_strdup_printf("/soc/iommu@%" PRIx64,
                                                iommu_addr);

        qemu_fdt_add_subnode(ms->fdt, name);
        qemu_fdt_setprop_string(ms->fdt, name, "compatible", "riscv,iommu");
        qemu_fdt_setprop_cells(ms->fdt, name, "reg",
                               (uint32_t)(iommu_addr >> 32),
                               (uint32_t)iommu_addr,
                               (uint32_t)(iommu_size >> 32),
                               (uint32_t)iommu_size);
        qemu_fdt_setprop_cell(ms->fdt, name, "#iommu-cells", 1);
        qemu_fdt_setprop_cell(ms->fdt, name, "phandle", iommu_ph);

        /* One wired IRQ per IOMMU: all causes share iommu_N_int via icvec=0 */
        if (aplic_s_phandle) {
            qemu_fdt_setprop_cell(ms->fdt, name, "interrupt-parent",
                                  aplic_s_phandle);
            qemu_fdt_setprop_cells(ms->fdt, name, "interrupts",
                                   base_irq, 0x4 /* IRQ_TYPE_LEVEL_HIGH */);
        }

        iommu_phandles[p] = iommu_ph;
    }
}

/* ---------------------------------------------------------------------------
 * Top-level: beta_create_fdt()
 * Called from beta.c beta_boot_setup().
 * -------------------------------------------------------------------------*/

void beta_create_fdt(BetaMachineState *s)
{
    MachineState *ms = MACHINE(s);
    BetaSoCConfig *cfg = s->config;
    BetaIMSICConfig *icfg = &cfg->interrupts.imsic;
    uint32_t num_acpu = cfg->acpu.num_clusters * cfg->acpu.cores_per_cluster;
    uint32_t acpu_hartid_base = 0;  /* ACPU hartid 0-31 */
    uint32_t phandle = 1;
    uint32_t *intc_phandles;
    uint32_t msi_m_phandle, msi_s_phandle;
    uint32_t aplic_root_phandle, aplic_m_phandle, aplic_s_phandle;
    uint32_t iommu_phandles[4] = { 0 };
    uint32_t guest_bits;
    int fdt_size;

    ms->fdt = create_device_tree(&fdt_size);
    if (!ms->fdt) {
        error_report("beta: create_device_tree() failed");
        exit(1);
    }

    /* Root node */
    qemu_fdt_setprop_string(ms->fdt, "/", "model", "Beta RISC-V SoC");
    qemu_fdt_setprop_string(ms->fdt, "/", "compatible", "riscv-beta,qemu");
    qemu_fdt_setprop_cell(ms->fdt, "/", "#size-cells", 2);
    qemu_fdt_setprop_cell(ms->fdt, "/", "#address-cells", 2);

    /* /soc bus */
    qemu_fdt_add_subnode(ms->fdt, "/soc");
    qemu_fdt_setprop(ms->fdt, "/soc", "ranges", NULL, 0);
    qemu_fdt_setprop_string(ms->fdt, "/soc", "compatible", "simple-bus");
    qemu_fdt_setprop_cell(ms->fdt, "/soc", "#size-cells", 2);
    qemu_fdt_setprop_cell(ms->fdt, "/soc", "#address-cells", 2);

    /* /chosen and /aliases — populated later */
    qemu_fdt_add_subnode(ms->fdt, "/chosen");
    if (ms->kernel_cmdline && *ms->kernel_cmdline) {
        qemu_fdt_setprop_string(ms->fdt, "/chosen", "bootargs",
                                ms->kernel_cmdline);
    }
    qemu_fdt_add_subnode(ms->fdt, "/aliases");

    /* /cpus — ACPU harts visible to Linux */
    intc_phandles = beta_fdt_create_cpus(s, &phandle);

    beta_fdt_create_memory(s);

    /* NUMA distance map (only for multi-cluster configs) */
    beta_fdt_create_numa_distance_map(s);

    /* ACLINT: MSWI + MTIMER */
    if (cfg->interrupts.clint.base) {
        beta_fdt_create_mswi(s, intc_phandles, num_acpu, acpu_hartid_base);
        beta_fdt_create_mtimer(s, intc_phandles, num_acpu, acpu_hartid_base);
    }

    /* IMSIC M/S */
    guest_bits = (icfg->num_vs_files > 0) ?
                 dtb_num_bits(icfg->num_vs_files + 1) : 0;

    msi_m_phandle = (icfg->m_base) ? phandle++ : 0;
    msi_s_phandle = (icfg->s_base) ? phandle++ : 0;

    if (icfg->m_base) {
        beta_fdt_create_imsic(s, intc_phandles, num_acpu,
                              true, 0, msi_m_phandle);
    }
    if (icfg->s_base) {
        beta_fdt_create_imsic(s, intc_phandles, num_acpu,
                              false, guest_bits, msi_s_phandle);
    }

    /*
     * 3-level APLIC: MLROOT → MLAPP → SLAPP
     * Create bottom-up so child phandles are available for parent.
     */
    aplic_s_phandle    = phandle++;  /* SLAPP */
    aplic_m_phandle    = phandle++;  /* MLAPP */
    aplic_root_phandle = phandle++;  /* MLROOT */

    /* SLAPP: S-level, MSI to IMSIC S, leaf (no children) */
    beta_fdt_create_aplic_domain(s, "s", aplic_s_phandle,
                                 msi_s_phandle, 0,
                                 NULL, 0, 0);

    /* MLAPP: M-level, MSI to IMSIC M, child=SLAPP */
    beta_fdt_create_aplic_domain(s, "m", aplic_m_phandle,
                                 msi_m_phandle, aplic_s_phandle,
                                 NULL, 0, 0);

    /* MLROOT: root, direct delivery to MCPU, child=MLAPP */
    {
        uint32_t mcpu_hartid = num_acpu; /* MCPU hartid = 32 */
        /*
         * For MLROOT direct delivery, we need the MCPU's intc phandle.
         * MCPU intc is not in intc_phandles[] (those are ACPU only).
         * Create a dummy intc phandle for MCPU — the actual MCPU
         * intc node would need to be added to FDT separately.
         * For now, skip direct delivery wiring in FDT (MCPU FDT
         * support deferred until MCPU boot flow is implemented).
         */
        (void)mcpu_hartid;
        beta_fdt_create_aplic_domain(s, "root", aplic_root_phandle,
                                     0, aplic_m_phandle,
                                     NULL, 0, 0);
    }

    beta_fdt_create_uart(s, aplic_s_phandle);

    /* IOMMU: one per PCIe subsystem, must be created before PCIe nodes */
    beta_fdt_create_iommu(s, aplic_s_phandle, iommu_phandles, &phandle);

    /*
     * PCIe FDT: two-level structure per subsystem
     *   rv-mgmt-pcie (parent) — management layer
     *     rv-gen4-pcie (child) — DWC controller (rv16 in x16 mode)
     *
     * Addresses from PCIe子系统说明.docx:
     *   mgmt = dbi_base + 0x5000000 (subsystem mgmt Cfg)
     *   appl = dbi_base + 0x6000000 (rv16 custom Cfg)
     *   atu  = dbi_base + 0x300000  (rv16 ATU)
     */
    for (uint32_t p = 0; p < cfg->num_pcie; p++) {
        BetaPCIeConfig *pcfg = &cfg->pcie[p];
        int num_ctrl = (pcfg->bifurcation == 4) ? 4 : 1;
        int j;

        if (pcfg->dbi_base == 0) {
            continue;
        }

        hwaddr mgmt_base = pcfg->dbi_base + 0x5000000;

        /* Parent: rv-mgmt-pcie */
        g_autofree char *mgmt_name =
            g_strdup_printf("/soc/pcie-mgmt@%" PRIx64, mgmt_base);

        qemu_fdt_add_subnode(ms->fdt, mgmt_name);
        qemu_fdt_setprop_string(ms->fdt, mgmt_name, "compatible",
                                "rivai,rv-mgmt-pcie");
        qemu_fdt_setprop_cells(ms->fdt, mgmt_name, "reg",
                               (uint32_t)(mgmt_base >> 32),
                               (uint32_t)mgmt_base,
                               0, 0x100000);  /* 1MB mgmt space */
        {
            static const char * const rn[] = { "mgmt" };
            qemu_fdt_setprop_string_array(ms->fdt, mgmt_name, "reg-names",
                                          (char **)rn, 1);
        }
        qemu_fdt_setprop_cell(ms->fdt, mgmt_name, "system-id", p);
        qemu_fdt_setprop_cell(ms->fdt, mgmt_name, "pcie-bifurcation",
                              num_ctrl);
        qemu_fdt_setprop_cell(ms->fdt, mgmt_name, "#address-cells", 2);
        qemu_fdt_setprop_cell(ms->fdt, mgmt_name, "#size-cells", 2);
        qemu_fdt_setprop(ms->fdt, mgmt_name, "ranges", NULL, 0);

        /*
         * Child nodes: rv-gen4-pcie controller(s)
         *
         * In x16 mode (bifurcation=1): single rv-gen4-pcie child.
         * In 4x4 mode (bifurcation=4): 4 independent rv-gen4-pcie
         * children, each with its own DBI/ATU/APPL/ECAM/MMIO slice.
         */
        for (j = 0; j < num_ctrl; j++) {
            hwaddr ctrl_dbi_base, ctrl_dbi_size;
            hwaddr appl_base, appl_size;
            hwaddr cfg_base, cfg_size;
            hwaddr p_base, np_base;
            hwaddr p_size, np_size;
            int bus_range_end;

            if (num_ctrl == 4) {
                /* 4x4 bifurcation: split resources by 4 */
                ctrl_dbi_base = pcfg->dbi_base + j * 0x40000;
                ctrl_dbi_size = MIN(pcfg->dbi_size, 0x40000);
                appl_base     = pcfg->dbi_base + 0x6000000
                                + j * 0x200000;
                appl_size     = 0x200000;    /* 2MB per controller */
                cfg_base      = pcfg->ecam_base
                                + j * (pcfg->ecam_size / 4);
                cfg_size      = pcfg->ecam_size ? pcfg->ecam_size / 4
                                                : 0x4000000;
                /*
                 * MMIO windows split 4 ways:
                 *   prefetchable: 128MB per controller
                 *   non-prefetchable: 64MB per controller
                 */
                p_base  = pcfg->mmio_base + 0x10000000
                          + j * 0x8000000;
                p_size  = 0x8000000;         /* 128MB */
                np_base = pcfg->mmio_base + 0x30000000
                          + j * 0x4000000;
                np_size = 0x4000000;         /* 64MB */
                bus_range_end = 0x3f;        /* 64 buses per controller */
            } else {
                /* x16: original single-controller layout */
                ctrl_dbi_base = pcfg->dbi_base;
                ctrl_dbi_size = pcfg->dbi_size;
                appl_base     = pcfg->dbi_base + 0x6000000;
                appl_size     = 0x800000;
                cfg_base      = pcfg->ecam_base;
                cfg_size      = pcfg->ecam_size ? pcfg->ecam_size
                                                : 0x10000000;
                p_base        = pcfg->mmio_base + 0x10000000;
                p_size        = 0x20000000;  /* 512MB */
                np_base       = pcfg->mmio_base + 0x30000000;
                np_size       = 0x10000000;  /* 256MB */
                bus_range_end = 0xff;
            }

            g_autofree char *pcie_name =
                g_strdup_printf("%s/pcie@%" PRIx64,
                                mgmt_name, ctrl_dbi_base);

            qemu_fdt_add_subnode(ms->fdt, pcie_name);
            qemu_fdt_setprop_string(ms->fdt, pcie_name, "compatible",
                                    "rivai,rv-gen4-pcie");
            qemu_fdt_setprop_string(ms->fdt, pcie_name,
                                    "device_type", "pci");
            qemu_fdt_setprop_cell(ms->fdt, pcie_name,
                                  "#address-cells", 3);
            qemu_fdt_setprop_cell(ms->fdt, pcie_name,
                                  "#size-cells", 2);
            qemu_fdt_setprop_cell(ms->fdt, pcie_name,
                                  "#interrupt-cells", 1);
            qemu_fdt_setprop_cell(ms->fdt, pcie_name,
                                  "controller-id", j);

            /* reg: dbi + appl + config */
            qemu_fdt_setprop_cells(ms->fdt, pcie_name, "reg",
                (uint32_t)(ctrl_dbi_base >> 32),
                (uint32_t)ctrl_dbi_base,
                (uint32_t)(ctrl_dbi_size >> 32),
                (uint32_t)ctrl_dbi_size,
                (uint32_t)(appl_base >> 32),
                (uint32_t)appl_base,
                0, (uint32_t)appl_size,
                (uint32_t)(cfg_base >> 32),
                (uint32_t)cfg_base,
                (uint32_t)(cfg_size >> 32),
                (uint32_t)cfg_size);
            {
                static const char * const rn[] = {
                    "dbi", "appl", "config"
                };
                qemu_fdt_setprop_string_array(ms->fdt, pcie_name,
                                              "reg-names",
                                              (char **)rn, 3);
            }

            /* ranges: PCIe MMIO windows */
            if (pcfg->mmio_base) {
                /*
                 * Non-prefetchable PCI address must be <4GB (PCI bridge
                 * Memory Base/Limit is 32-bit only). CPU address stays
                 * at the real >4GB location; DWC iATU translates.
                 */
                uint32_t np_pci_base = (uint32_t)np_base;  /* low 32 bits */
                qemu_fdt_setprop_cells(ms->fdt, pcie_name, "ranges",
                    /* prefetchable 64-bit MEM (PCI addr = CPU addr, 1:1) */
                    0x43000000,
                    (uint32_t)(p_base >> 32), (uint32_t)p_base,
                    (uint32_t)(p_base >> 32), (uint32_t)p_base,
                    0, (uint32_t)p_size,
                    /* non-prefetchable 32-bit MEM (PCI addr <4GB, CPU addr >4GB) */
                    0x02000000,
                    0, np_pci_base,
                    (uint32_t)(np_base >> 32), (uint32_t)np_base,
                    0, (uint32_t)np_size);
            }

            /* Interrupts: each controller gets its own IRQ */
            if (aplic_s_phandle) {
                int ctrl_irq = 0;
                /*
                 * IRQ lookup: pcie{subsys}_{j*4} for 4x4 mode,
                 * pcie{subsys}_0 for x16 mode.
                 */
                char irq_name[32];
                /* New IRQ table: pcie{N}_intx is the base of INTA-INTD for
                 * controller N. Each subsystem has 4 IRQs (IRQ 28-31, 32-35,
                 * 36-39, 40-43) regardless of x16/4x4 mode. */
                snprintf(irq_name, sizeof(irq_name), "pcie%u_intx", p);
                for (uint32_t k = 0; k < cfg->interrupts.num_irqs; k++) {
                    if (g_strcmp0(cfg->interrupts.irq_map[k].name,
                                  irq_name) == 0) {
                        ctrl_irq = cfg->interrupts.irq_map[k].irq_num;
                        break;
                    }
                }
                if (ctrl_irq > 0) {
                    /*
                     * Child interrupt-controller node.
                     *
                     * rv-gen4-pcie driver calls
                     *   of_get_child_by_name(np, "interrupt-controller")
                     * and creates a linear irq_domain (4 slots × 4 pins)
                     * over it.  Without this node the driver prints:
                     *   "rv gen4 pcie get interrupt-controller node error"
                     * and returns -EINVAL from init_irq_domain, leaving
                     * the PCIe bus with no IRQ domain.  TX completions
                     * (MSI or INTx) never fire → virtio TX watchdog fires.
                     *
                     * The interrupt-map below routes PCI INTx through this
                     * child domain (not directly to APLIC); the DWC driver
                     * chains ctrl_irq → child domain demux internally.
                     */
                    uint32_t pcie_intc_phandle = phandle++;
                    {
                        g_autofree char *intc_name =
                            g_strdup_printf("%s/interrupt-controller",
                                            pcie_name);
                        /*
                         * qemu_fdt_add_path() creates missing path
                         * components but is a no-op for existing nodes,
                         * avoiding FDT_ERR_EXISTS / exit(1) if the node
                         * was already created by another code path.
                         */
                        qemu_fdt_add_path(ms->fdt, intc_name);
                        qemu_fdt_setprop_cell(ms->fdt, intc_name,
                                              "#address-cells", 0);
                        qemu_fdt_setprop_cell(ms->fdt, intc_name,
                                              "#interrupt-cells", 1);
                        qemu_fdt_setprop(ms->fdt, intc_name,
                                         "interrupt-controller", NULL, 0);
                        qemu_fdt_setprop_cell(ms->fdt, intc_name,
                                              "phandle", pcie_intc_phandle);
                    }

                    qemu_fdt_setprop_cell(ms->fdt, pcie_name,
                                          "interrupt-parent",
                                          aplic_s_phandle);
                    qemu_fdt_setprop_cells(ms->fdt, pcie_name,
                                           "interrupts",
                                           ctrl_irq, 0x4);
                    /*
                     * rv-gen4-pcie driver uses of_irq_get_byname(node, "legacy")
                     * to get the controller's aggregate INTx interrupt line.
                     */
                    qemu_fdt_setprop_string(ms->fdt, pcie_name,
                                            "interrupt-names", "legacy");

                    /*
                     * interrupt-map: standard PCI swizzle routing INTA-INTD
                     * from 4 PCI slots through the child interrupt-controller
                     * domain (pcie_intc_phandle, #interrupt-cells=1).
                     *
                     * Formula: intc_irq = (pin-1 + dev) % PCI_NUM_PINS
                     *   → each slot rotates the pin assignment by one.
                     *
                     * Entry layout (6 cells):
                     *   [child-addr(3)] [pin(1)] [phandle(1)] [intc-irq(1)]
                     */
                    {
                        uint32_t irq_map[4 * 4 * 6];
                        int idx = 0;
                        int dev, pin;
                        for (dev = 0; dev < 4; dev++) {
                            for (pin = 1; pin <= 4; pin++) {
                                int intc_irq = (pin - 1 + dev) % 4;
                                /* PCI address: devfn<<8, 0, 0 */
                                irq_map[idx++] = cpu_to_be32((dev << 3) << 8);
                                irq_map[idx++] = cpu_to_be32(0);
                                irq_map[idx++] = cpu_to_be32(0);
                                /* PCI pin (1=INTA .. 4=INTD) */
                                irq_map[idx++] = cpu_to_be32(pin);
                                /* Parent: child interrupt-controller */
                                irq_map[idx++] = cpu_to_be32(pcie_intc_phandle);
                                /* Sub-IRQ (0-3) within the child domain */
                                irq_map[idx++] = cpu_to_be32(intc_irq);
                            }
                        }
                        qemu_fdt_setprop(ms->fdt, pcie_name,
                                         "interrupt-map",
                                         irq_map, sizeof(irq_map));
                        /* Mask: device bits [12:11] + pin bits [2:0] */
                        qemu_fdt_setprop_cells(ms->fdt, pcie_name,
                                               "interrupt-map-mask",
                                               0x1800, 0, 0, 7);
                    }
                }
            }

            qemu_fdt_setprop_cells(ms->fdt, pcie_name, "bus-range",
                                   (num_ctrl == 4) ? j * 0x40 : 0,
                                   (num_ctrl == 4) ? j * 0x40 + bus_range_end
                                                   : bus_range_end);
            qemu_fdt_setprop_cell(ms->fdt, pcie_name,
                                  "linux,pci-domain",
                                  (num_ctrl == 4) ? p * 4 + j : p);

            /*
             * iommu-map intentionally omitted: QEMU's RISC-V IOMMU is not
             * wired into the DWC PCIe DMA path.  With iommu-map present the
             * kernel allocates IOVAs and passes them to virtio as DMA addresses;
             * since the IOMMU does no translation the device uses IOVAs as
             * physical addresses → DMA corruption.  Omitting iommu-map makes
             * the kernel use 1:1 DMA mappings, which work correctly in QEMU.
             * (Platform Guide §14.1)
             */

            /*
             * MSI via IMSIC S-level.
             *
             * Use msi-parent ONLY — do NOT add msi-map.
             *
             * When msi-map is present Linux calls of_msi_map_get_device_domain()
             * which searches for DOMAIN_BUS_PCI_MSI.  The RISC-V IMSIC driver
             * registers its MSI parent domain under DOMAIN_BUS_NEXUS, not
             * DOMAIN_BUS_PCI_MSI, so the lookup returns NULL → EINVAL (-22).
             *
             * With only msi-parent, Linux calls pci_host_bridge_of_msi_domain()
             * which falls through to DOMAIN_BUS_NEXUS and succeeds, matching
             * the behaviour of the reference virt.c PCIe node (hw/riscv/virt.c
             * create_fdt_pcie() uses msi-parent only, no msi-map).
             */
            if (msi_s_phandle) {
                qemu_fdt_setprop_cell(ms->fdt, pcie_name,
                                      "msi-parent", msi_s_phandle);
            }

            /* QEMU memory is always coherent */
            qemu_fdt_setprop(ms->fdt, pcie_name,
                             "dma-coherent", NULL, 0);
        }
    }

    g_free(intc_phandles);
}

/* ===========================================================================
 * MCPU FDT — separate device tree for the RV32 management processor.
 *
 * MCPU runs its own management OS with its own view of hardware:
 *   /cpus:     1 RV32IMAC hart (hartid 32)
 *   /memory:   MCPU SRAM @ 0xF0000000 (512KB)
 *   /soc:      MLROOT APLIC (direct delivery), UART, SOC_TOP_CSR alias
 *
 * The FDT blob is placed at the end of MCPU SRAM.
 * ===========================================================================*/

void beta_create_mcpu_fdt(BetaMachineState *s)
{
    BetaSoCConfig *cfg = s->config;
    BetaMCPUConfig *mcpu = &cfg->mcpu;
    BetaAPLICConfig *acfg = &cfg->interrupts.aplic;
    BetaCLINTConfig *clint = &cfg->interrupts.clint;
    uint32_t num_acpu = cfg->acpu.num_clusters * cfg->acpu.cores_per_cluster;
    uint32_t mcpu_hartid = num_acpu;  /* hartid 32 */
    uint32_t timebase_freq = clint->timebase_freq ?
                             clint->timebase_freq :
                             RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ;
    uint32_t phandle = 1;
    uint32_t intc_phandle;
    uint32_t aplic_root_phandle;
    int fdt_size;
    void *fdt;

    fdt = create_device_tree(&fdt_size);
    if (!fdt) {
        error_report("beta: create_device_tree() failed for MCPU FDT");
        return;
    }

    /* Root node */
    qemu_fdt_setprop_string(fdt, "/", "model", "Beta MCPU Management Processor");
    qemu_fdt_setprop_string(fdt, "/", "compatible", "riscv-beta-mcpu,qemu");
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 1);
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 1);

    /* /chosen */
    qemu_fdt_add_subnode(fdt, "/chosen");

    /* /aliases */
    qemu_fdt_add_subnode(fdt, "/aliases");

    /*
     * /cpus — single MCPU hart
     */
    intc_phandle = phandle++;

    qemu_fdt_add_subnode(fdt, "/cpus");
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 1);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0);
    qemu_fdt_setprop_cell(fdt, "/cpus", "timebase-frequency", timebase_freq);

    {
        const char *isa = mcpu->isa ? mcpu->isa : "rv32imac";
        g_autofree char *cpu_name =
            g_strdup_printf("/cpus/cpu@%u", mcpu_hartid);
        g_autofree char *intc_name =
            g_strdup_printf("/cpus/cpu@%u/interrupt-controller", mcpu_hartid);

        qemu_fdt_add_subnode(fdt, cpu_name);
        qemu_fdt_setprop_string(fdt, cpu_name, "device_type", "cpu");
        qemu_fdt_setprop_cell(fdt, cpu_name, "reg", mcpu_hartid);
        qemu_fdt_setprop_string(fdt, cpu_name, "compatible", "riscv");
        qemu_fdt_setprop_string(fdt, cpu_name, "status", "okay");
        qemu_fdt_setprop_string(fdt, cpu_name, "riscv,isa", isa);
        qemu_fdt_setprop_string(fdt, cpu_name, "riscv,isa-base", "rv32i");

        /* interrupt-controller sub-node */
        qemu_fdt_add_subnode(fdt, intc_name);
        qemu_fdt_setprop_string(fdt, intc_name, "compatible",
                                "riscv,cpu-intc");
        qemu_fdt_setprop(fdt, intc_name, "interrupt-controller", NULL, 0);
        qemu_fdt_setprop_cell(fdt, intc_name, "#interrupt-cells", 1);
        qemu_fdt_setprop_cell(fdt, intc_name, "phandle", intc_phandle);
    }

    /*
     * /memory — MCPU SRAM
     * MCPU uses SRAM as its main working memory (no DDR access).
     */
    {
        hwaddr sram_base = 0, sram_size = 0;
        uint32_t i;
        for (i = 0; i < cfg->num_memory_regions; i++) {
            if (g_strcmp0(cfg->memory_regions[i].name, "mcpu_sram") == 0) {
                sram_base = cfg->memory_regions[i].base;
                sram_size = cfg->memory_regions[i].size;
                break;
            }
        }
        if (sram_base && sram_size) {
            g_autofree char *name =
                g_strdup_printf("/memory@%" PRIx64, sram_base);
            qemu_fdt_add_subnode(fdt, name);
            qemu_fdt_setprop_string(fdt, name, "device_type", "memory");
            qemu_fdt_setprop_cells(fdt, name, "reg",
                                   (uint32_t)sram_base, (uint32_t)sram_size);
        }
    }

    /* /soc bus */
    qemu_fdt_add_subnode(fdt, "/soc");
    qemu_fdt_setprop(fdt, "/soc", "ranges", NULL, 0);
    qemu_fdt_setprop_string(fdt, "/soc", "compatible", "simple-bus");
    qemu_fdt_setprop_cell(fdt, "/soc", "#size-cells", 1);
    qemu_fdt_setprop_cell(fdt, "/soc", "#address-cells", 1);

    /*
     * MLROOT APLIC — direct delivery to MCPU
     */
    aplic_root_phandle = phandle++;
    {
        BetaAPLICDomain *root_domain = NULL;
        uint32_t num_sources = acfg->num_sources ? acfg->num_sources : 1023;
        uint32_t i;

        for (i = 0; i < acfg->num_domains; i++) {
            if (g_strcmp0(acfg->domains[i].level, "root") == 0) {
                root_domain = &acfg->domains[i];
                break;
            }
        }

        if (root_domain && root_domain->base) {
            hwaddr base = root_domain->base;
            hwaddr size = root_domain->size ? root_domain->size : 0x10000;
            g_autofree char *name =
                g_strdup_printf("/soc/interrupt-controller@%x", (uint32_t)base);

            qemu_fdt_add_subnode(fdt, name);
            qemu_fdt_setprop_string(fdt, name, "compatible", "riscv,aplic");
            qemu_fdt_setprop_cell(fdt, name, "#address-cells", 0);
            qemu_fdt_setprop_cell(fdt, name, "#interrupt-cells", 2);
            qemu_fdt_setprop(fdt, name, "interrupt-controller", NULL, 0);
            qemu_fdt_setprop_cells(fdt, name, "reg",
                                   (uint32_t)base, (uint32_t)size);
            qemu_fdt_setprop_cell(fdt, name, "riscv,num-sources",
                                  num_sources);

            /* Direct delivery: interrupts-extended to MCPU intc */
            {
                uint32_t cells[2];
                cells[0] = cpu_to_be32(intc_phandle);
                cells[1] = cpu_to_be32(IRQ_M_EXT);
                qemu_fdt_setprop(fdt, name, "interrupts-extended",
                                 cells, sizeof(cells));
            }

            qemu_fdt_setprop_cell(fdt, name, "phandle", aplic_root_phandle);
        }
    }

    /*
     * UART — shared with ACPU, MCPU sees it at same address
     */
    {
        BetaDeviceConfig *uart = NULL;
        uint32_t i;
        for (i = 0; i < cfg->num_devices; i++) {
            if (g_strcmp0(cfg->devices[i].type, "uart") == 0) {
                uart = &cfg->devices[i];
                break;
            }
        }
        if (uart && uart->base) {
            hwaddr base = uart->base;
            hwaddr size = uart->size ? uart->size : 0x1000;
            int irq_num = 0;
            g_autofree char *name =
                g_strdup_printf("/soc/serial@%x", (uint32_t)base);

            /* Find UART IRQ */
            for (i = 0; i < cfg->interrupts.num_irqs; i++) {
                if (g_strcmp0(cfg->interrupts.irq_map[i].name,
                              uart->irq_name) == 0) {
                    irq_num = cfg->interrupts.irq_map[i].irq_num;
                    break;
                }
            }

            qemu_fdt_add_subnode(fdt, name);
            qemu_fdt_setprop_string(fdt, name, "compatible", "ns16550a");
            qemu_fdt_setprop_cells(fdt, name, "reg",
                                   (uint32_t)base, (uint32_t)size);
            qemu_fdt_setprop_cell(fdt, name, "reg-shift", 2);
            qemu_fdt_setprop_cell(fdt, name, "reg-io-width", 1);
            qemu_fdt_setprop_cell(fdt, name, "clock-frequency",
                                  uart->clock_hz ? uart->clock_hz : 3686400);

            if (irq_num > 0 && aplic_root_phandle) {
                qemu_fdt_setprop_cell(fdt, name, "interrupt-parent",
                                      aplic_root_phandle);
                qemu_fdt_setprop_cells(fdt, name, "interrupts",
                                       irq_num, 0x4);
            }

            qemu_fdt_setprop_string(fdt, "/chosen", "stdout-path", name);
            qemu_fdt_setprop_string(fdt, "/aliases", "serial0", name);
        }
    }

    /*
     * Platform Timer (CLINT) @ 0xE004_1000
     * MCPU's view of the shared CLINT. Standard ACLINT layout:
     *   MSWI:   base + 0x0000 (soft_int)
     *   MTIMER: base + 0x4000 (mtimecmp + mtime)
     * 64-bit registers accessed as high/low 32-bit pairs on RV32.
     */
    {
        hwaddr timer_base = 0xE0041000;

        /* MSWI node */
        {
            g_autofree char *name =
                g_strdup_printf("/soc/mswi@%x", (uint32_t)timer_base);
            qemu_fdt_add_subnode(fdt, name);
            qemu_fdt_setprop_string(fdt, name, "compatible",
                                    "riscv,aclint-mswi");
            qemu_fdt_setprop_cells(fdt, name, "reg",
                                   (uint32_t)timer_base, 0x4000);
            qemu_fdt_setprop(fdt, name, "interrupts-extended",
                             (uint32_t[]){
                                 cpu_to_be32(intc_phandle),
                                 cpu_to_be32(IRQ_M_SOFT)
                             }, 2 * sizeof(uint32_t));
        }

        /* MTIMER node */
        {
            hwaddr mtimer_base = timer_base + 0x4000;
            g_autofree char *name =
                g_strdup_printf("/soc/mtimer@%x", (uint32_t)mtimer_base);
            qemu_fdt_add_subnode(fdt, name);
            qemu_fdt_setprop_string(fdt, name, "compatible",
                                    "riscv,aclint-mtimer");
            qemu_fdt_setprop_cells(fdt, name, "reg",
                                   (uint32_t)mtimer_base, 0x8000);
            qemu_fdt_setprop(fdt, name, "interrupts-extended",
                             (uint32_t[]){
                                 cpu_to_be32(intc_phandle),
                                 cpu_to_be32(IRQ_M_TIMER)
                             }, 2 * sizeof(uint32_t));
        }
    }

    /*
     * ext_addr CSR @ 0xE004_D000
     * 4KB register block; register at offset 0xC sets the upper address
     * bits for AHB master window accesses (40-bit physical addressing).
     */
    {
        g_autofree char *name =
            g_strdup_printf("/soc/ext-addr-csr@%x", 0xe004d000);
        qemu_fdt_add_subnode(fdt, name);
        qemu_fdt_setprop_string(fdt, name, "compatible",
                                "riscv-beta,mcpu-ext-addr");
        qemu_fdt_setprop_cells(fdt, name, "reg",
                               0xe004d000, 0x1000);
    }

    /*
     * AHB master window @ 0xA000_0000
     * 256MB window; accesses are translated to full 40-bit addresses
     * using the upper bits programmed via the ext_addr register.
     */
    {
        g_autofree char *name =
            g_strdup_printf("/soc/ahb-master@%x", 0xa0000000);
        qemu_fdt_add_subnode(fdt, name);
        qemu_fdt_setprop_string(fdt, name, "compatible",
                                "riscv-beta,mcpu-ahb-window");
        qemu_fdt_setprop_cells(fdt, name, "reg",
                               0xa0000000, 0x10000000);
    }

    s->mcpu_fdt = fdt;
}
