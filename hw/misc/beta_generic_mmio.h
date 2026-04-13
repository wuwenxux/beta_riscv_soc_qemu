/*
 * Beta SoC - Config-driven Generic MMIO Register File Device
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#ifndef HW_BETA_GENERIC_MMIO_H
#define HW_BETA_GENERIC_MMIO_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define TYPE_BETA_GENERIC_MMIO "beta-generic-mmio"
OBJECT_DECLARE_SIMPLE_TYPE(BetaGenericMMIOState, BETA_GENERIC_MMIO)

/* Register access types */
typedef enum {
    BETA_REG_RW = 0,    /* Read-Write */
    BETA_REG_RO,        /* Read-Only */
    BETA_REG_WO,        /* Write-Only */
    BETA_REG_W1C,       /* Write-1-to-Clear */
    BETA_REG_RC,        /* Read-to-Clear */
} BetaRegAccessType;

/* Single register definition */
typedef struct BetaMMIORegDef {
    char *name;
    uint64_t offset;
    uint32_t width;          /* 1, 2, 4, or 8 bytes */
    uint64_t reset_value;
    BetaRegAccessType access;
} BetaMMIORegDef;

/* Write callback function type */
typedef void (*BetaMMIOWriteCallback)(void *opaque, const char *reg_name,
                                      uint64_t value);

/* Write callback entry */
typedef struct BetaMMIOCallback {
    char *reg_name;
    BetaMMIOWriteCallback fn;
    void *opaque;
} BetaMMIOCallback;

struct BetaGenericMMIOState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    qemu_irq irq;

    char *device_name;

    /* Register definitions (from config) */
    BetaMMIORegDef *reg_defs;
    uint32_t num_regs;

    /* Current register values */
    uint64_t *reg_values;

    /* Write callbacks for side-effect registers */
    BetaMMIOCallback *callbacks;
    uint32_t num_callbacks;

    /* Total MMIO region size */
    uint64_t region_size;
};

/* Public API */
void beta_generic_mmio_add_callback(BetaGenericMMIOState *s,
                                    const char *reg_name,
                                    BetaMMIOWriteCallback fn,
                                    void *opaque);
void beta_generic_mmio_set_reg_defs(BetaGenericMMIOState *s,
                                    BetaMMIORegDef *defs,
                                    uint32_t num_regs,
                                    uint64_t region_size);
uint64_t beta_generic_mmio_get_reg(BetaGenericMMIOState *s,
                                   const char *reg_name);
void beta_generic_mmio_set_reg(BetaGenericMMIOState *s,
                               const char *reg_name, uint64_t value);

#endif /* HW_BETA_GENERIC_MMIO_H */
