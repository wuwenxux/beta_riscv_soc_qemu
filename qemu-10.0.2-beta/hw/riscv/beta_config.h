/*
 * Beta RISC-V SoC Configuration Loader
 *
 * Copyright (c) 2024-2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef BETA_CONFIG_H
#define BETA_CONFIG_H

#include "qemu/osdep.h"
#include "qapi/error.h"

typedef struct BetaRegFieldDef {
    char *name;
    uint32_t hi_bit;
    uint32_t lo_bit;
    char *access;      /* "rw", "ro", "wo", "w1c", "rc" */
    uint64_t reset;
} BetaRegFieldDef;

typedef struct BetaRegDef {
    char *name;
    uint64_t offset;
    uint32_t width;    /* 8, 16, 32, 64 */
    uint64_t reset_value;
    char *access;      /* "rw", "ro", "wo", "w1c", "rc" */
    BetaRegFieldDef *fields;
    uint32_t num_fields;
} BetaRegDef;

typedef struct BetaCacheConfig {
    uint32_t l1i_size_kb;
    uint32_t l1i_ways;
    uint32_t l1d_size_kb;
    uint32_t l1d_ways;
    uint32_t l2_size_kb;
    uint32_t l2_ways;
    bool l2_inclusive;
    uint32_t l3_slice_kb;
    uint32_t l3_num_slices;
    uint32_t line_bytes;
} BetaCacheConfig;

typedef struct BetaACPUConfig {
    uint32_t num_clusters;
    uint32_t cores_per_cluster;
    uint32_t hartid_base;   /* hart ID of the first ACPU core (default 0) */
    char *isa;
    char *mmu_mode;     /* "sv39", "sv48", etc. */
    char *pma_type;     /* "svpbmt", "sam", "none" */
    uint64_t reset_vector;
    BetaCacheConfig cache;
    uint32_t pmu_fixed_counters;
    uint32_t pmu_prog_counters;
    bool pmu_sscofpmf;
    /* Machine identification CSRs (BUG-P0-03) */
    uint32_t mvendorid;
    uint64_t marchid;
    uint64_t mimpid;
} BetaACPUConfig;

typedef struct BetaMCPUConfig {
    uint32_t num_cores;
    char *isa;
    uint32_t itcm_kb;
    uint32_t dtcm_kb;
    uint32_t sram_kb;
    uint32_t icache_kb;
    uint32_t icache_ways;
    uint64_t reset_vector;
    uint32_t hartid_base;   /* hart ID of first MCPU core; 0 = auto (= num_acpu) */
} BetaMCPUConfig;

typedef struct BetaMemRegion {
    char *name;
    uint64_t base;
    uint64_t size;
    char *type;         /* "ram", "rom", "mmio", "pcie_ecam", etc. */
    char *cpu_visible;  /* "acpu", "mcpu", "both" */
} BetaMemRegion;

typedef struct BetaIRQEntry {
    char *name;
    uint32_t irq_num;
    char *target;       /* "aplic", "plic" */
    char *domain;       /* "m_root", "m_app", "s_app" */
} BetaIRQEntry;

typedef struct BetaAPLICDomain {
    char *name;
    char *level;        /* "m", "s" */
    uint64_t base;
    uint64_t size;
    char *parent;
} BetaAPLICDomain;

typedef struct BetaAPLICConfig {
    uint32_t num_sources;
    BetaAPLICDomain *domains;
    uint32_t num_domains;
} BetaAPLICConfig;

typedef struct BetaIMSICConfig {
    uint64_t m_base;
    uint64_t s_base;
    uint32_t guest_index_bits;
    uint32_t num_ids;
    uint32_t num_vs_files;
} BetaIMSICConfig;

typedef struct BetaCLINTConfig {
    uint64_t base;
    uint64_t size;
    uint32_t timebase_freq;
} BetaCLINTConfig;

typedef struct BetaPLICConfig {
    uint64_t base;
    uint64_t size;
    uint32_t num_sources;
    uint32_t num_priorities;
} BetaPLICConfig;

typedef struct BetaInterruptConfig {
    BetaAPLICConfig aplic;
    BetaIMSICConfig imsic;
    BetaCLINTConfig clint;
    BetaPLICConfig plic;
    BetaIRQEntry *irq_map;
    uint32_t num_irqs;
} BetaInterruptConfig;

typedef struct BetaDeviceConfig {
    char *name;
    char *type;         /* "uart", "spi", "i2c", "generic_mmio", etc. */
    uint64_t base;
    uint64_t size;
    char *cpu_target;   /* "acpu", "mcpu", "both" */
    char *compatible;   /* DT compatible string */
    char *irq_name;     /* key into irq_map */
    uint32_t clock_hz;
    int instance_id;
    BetaRegDef *registers;
    uint32_t num_registers;
} BetaDeviceConfig;

typedef struct BetaDDRChannel {
    uint32_t id;
    uint64_t ctrl_base;
    uint64_t mem_base;
    uint64_t size;
} BetaDDRChannel;

typedef struct BetaDDRConfig {
    uint32_t num_channels;
    uint32_t channel_width_bits;
    uint32_t speed_mbps;
    uint32_t max_per_channel_gb;
    uint32_t interleave_step_bytes;
    BetaDDRChannel *channels;
} BetaDDRConfig;

typedef struct BetaBootConfig {
    char *mode;         /* "mcpu_bootrom", "mcpu_xip", "acpu_xip" */
    uint64_t bootrom_base;
    uint64_t bootrom_size;
    uint64_t flash_base;
    uint64_t flash_size;
    uint64_t mcpu_entry;
    uint64_t acpu_entry;
} BetaBootConfig;

typedef struct BetaPCIeConfig {
    uint64_t dbi_base;      /* DWC DBI register base */
    uint64_t dbi_size;
    uint64_t ecam_base;     /* ECAM config space base */
    uint64_t ecam_size;
    uint64_t mmio_base;     /* 32-bit MMIO window */
    uint64_t mmio_size;
    uint64_t mmio64_base;   /* 64-bit prefetchable MMIO */
    uint64_t mmio64_size;
    uint32_t num_lanes;     /* 1, 2, 4, 8, 16 */
    int bifurcation;        /* 1 = x16 (default), 4 = 4x4 bifurcation */
    char *irq_name;         /* MSI IRQ name */
    char *irq_intx_name;    /* INTx IRQ name */
    uint64_t iommu_base;    /* IOMMU register base (0 = no IOMMU) */
    int iommu_base_irq;     /* IOMMU base IRQ number in APLIC */
} BetaPCIeConfig;

typedef struct BetaSoCConfig {
    char *version;
    BetaACPUConfig acpu;
    BetaMCPUConfig mcpu;
    BetaMemRegion *memory_regions;
    uint32_t num_memory_regions;
    BetaInterruptConfig interrupts;
    BetaDeviceConfig *devices;
    uint32_t num_devices;
    BetaDDRConfig ddr;
    BetaBootConfig boot;
    BetaPCIeConfig *pcie;
    uint32_t num_pcie;
} BetaSoCConfig;

/**
 * beta_config_load: Load and parse a Beta SoC JSON configuration file.
 *
 * @filename: path to the JSON configuration file
 * @errp: pointer to error object
 *
 * Returns a newly allocated BetaSoCConfig on success, or NULL on error
 * with @errp set.  The caller must free with beta_config_free().
 */
BetaSoCConfig *beta_config_load(const char *filename, Error **errp);

/**
 * beta_config_free: Free a BetaSoCConfig and all owned memory.
 *
 * @config: config to free, may be NULL
 */
void beta_config_free(BetaSoCConfig *config);

#endif /* BETA_CONFIG_H */
