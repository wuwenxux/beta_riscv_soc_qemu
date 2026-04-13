/*
 * Beta SoC - Config-driven Generic MMIO Register File Device
 *
 * A fully configurable MMIO device whose register map is defined
 * externally via JSON configuration. Supports RW/RO/WO/W1C/RC
 * access types and optional write callbacks for side-effect registers.
 *
 * Copyright (c) 2026
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/misc/beta_generic_mmio.h"
#include "migration/vmstate.h"

static int find_reg_by_offset(BetaGenericMMIOState *s, hwaddr addr)
{
    for (uint32_t i = 0; i < s->num_regs; i++) {
        if (s->reg_defs[i].offset == addr) {
            return i;
        }
    }
    return -1;
}

static int find_reg_by_name(BetaGenericMMIOState *s, const char *name)
{
    for (uint32_t i = 0; i < s->num_regs; i++) {
        if (s->reg_defs[i].name && strcmp(s->reg_defs[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static BetaMMIOCallback *find_callback(BetaGenericMMIOState *s,
                                       const char *reg_name)
{
    for (uint32_t i = 0; i < s->num_callbacks; i++) {
        if (strcmp(s->callbacks[i].reg_name, reg_name) == 0) {
            return &s->callbacks[i];
        }
    }
    return NULL;
}

static uint64_t beta_generic_mmio_read(void *opaque, hwaddr addr,
                                       unsigned size)
{
    BetaGenericMMIOState *s = BETA_GENERIC_MMIO(opaque);
    int idx = find_reg_by_offset(s, addr);
    uint64_t val;

    if (idx < 0) {
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented read at offset 0x%" HWADDR_PRIx "\n",
                      s->device_name ? s->device_name : "beta-mmio", addr);
        return 0;
    }

    BetaMMIORegDef *reg = &s->reg_defs[idx];

    switch (reg->access) {
    case BETA_REG_WO:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: read from write-only register %s (0x%" HWADDR_PRIx ")\n",
                      s->device_name ? s->device_name : "beta-mmio",
                      reg->name ? reg->name : "?", addr);
        return 0;

    case BETA_REG_RC:
        val = s->reg_values[idx];
        s->reg_values[idx] = 0;
        return val;

    case BETA_REG_RW:
    case BETA_REG_RO:
    case BETA_REG_W1C:
    default:
        return s->reg_values[idx];
    }
}

static void beta_generic_mmio_write(void *opaque, hwaddr addr,
                                    uint64_t val, unsigned size)
{
    BetaGenericMMIOState *s = BETA_GENERIC_MMIO(opaque);
    int idx = find_reg_by_offset(s, addr);

    if (idx < 0) {
        qemu_log_mask(LOG_UNIMP,
                      "%s: unimplemented write at offset 0x%" HWADDR_PRIx
                      " value 0x%" PRIx64 "\n",
                      s->device_name ? s->device_name : "beta-mmio",
                      addr, val);
        return;
    }

    BetaMMIORegDef *reg = &s->reg_defs[idx];

    switch (reg->access) {
    case BETA_REG_RO:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to read-only register %s (0x%" HWADDR_PRIx ")\n",
                      s->device_name ? s->device_name : "beta-mmio",
                      reg->name ? reg->name : "?", addr);
        return;

    case BETA_REG_RC:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to read-clear register %s (0x%" HWADDR_PRIx ")\n",
                      s->device_name ? s->device_name : "beta-mmio",
                      reg->name ? reg->name : "?", addr);
        return;

    case BETA_REG_W1C:
        s->reg_values[idx] &= ~val;
        break;

    case BETA_REG_RW:
    case BETA_REG_WO:
    default:
        s->reg_values[idx] = val;
        break;
    }

    /* Check for write callbacks */
    if (reg->name) {
        BetaMMIOCallback *cb = find_callback(s, reg->name);
        if (cb && cb->fn) {
            cb->fn(cb->opaque, reg->name, val);
        }
    }
}

static const MemoryRegionOps beta_generic_mmio_ops = {
    .read = beta_generic_mmio_read,
    .write = beta_generic_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static void beta_generic_mmio_reset(DeviceState *dev)
{
    BetaGenericMMIOState *s = BETA_GENERIC_MMIO(dev);

    for (uint32_t i = 0; i < s->num_regs; i++) {
        s->reg_values[i] = s->reg_defs[i].reset_value;
    }
}

static void beta_generic_mmio_realize(DeviceState *dev, Error **errp)
{
    BetaGenericMMIOState *s = BETA_GENERIC_MMIO(dev);

    if ((s->num_regs == 0 || s->reg_defs == NULL) && s->region_size == 0) {
        /* No regs and no region size: skip MMIO region creation */
        return;
    }

    if (s->region_size == 0) {
        /* Calculate region size from highest register offset */
        uint64_t max_offset = 0;
        for (uint32_t i = 0; i < s->num_regs; i++) {
            uint64_t end = s->reg_defs[i].offset + s->reg_defs[i].width;
            if (end > max_offset) {
                max_offset = end;
            }
        }
        s->region_size = max_offset;
    }

    memory_region_init_io(&s->iomem, OBJECT(dev), &beta_generic_mmio_ops,
                          s, s->device_name ? s->device_name : "beta-mmio",
                          s->region_size);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    /* Allocate register value storage */
    s->reg_values = g_new0(uint64_t, s->num_regs);

    /* Apply reset values */
    beta_generic_mmio_reset(DEVICE(s));
}

static void beta_generic_mmio_init(Object *obj)
{
    BetaGenericMMIOState *s = BETA_GENERIC_MMIO(obj);
    s->num_regs = 0;
    s->reg_defs = NULL;
    s->reg_values = NULL;
    s->callbacks = NULL;
    s->num_callbacks = 0;
    s->device_name = NULL;
    s->region_size = 0;
}

static void beta_generic_mmio_finalize(Object *obj)
{
    BetaGenericMMIOState *s = BETA_GENERIC_MMIO(obj);

    g_free(s->reg_defs);
    g_free(s->reg_values);
    g_free(s->device_name);

    for (uint32_t i = 0; i < s->num_callbacks; i++) {
        g_free(s->callbacks[i].reg_name);
    }
    g_free(s->callbacks);
}

static void beta_generic_mmio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = beta_generic_mmio_realize;
    device_class_set_legacy_reset(dc, beta_generic_mmio_reset);
    dc->desc = "Beta SoC config-driven generic MMIO device";
}

static const TypeInfo beta_generic_mmio_typeinfo = {
    .name = TYPE_BETA_GENERIC_MMIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BetaGenericMMIOState),
    .instance_init = beta_generic_mmio_init,
    .instance_finalize = beta_generic_mmio_finalize,
    .class_init = beta_generic_mmio_class_init,
};

static void beta_generic_mmio_register_types(void)
{
    type_register_static(&beta_generic_mmio_typeinfo);
}
type_init(beta_generic_mmio_register_types)

/* Public API */

void beta_generic_mmio_set_reg_defs(BetaGenericMMIOState *s,
                                    BetaMMIORegDef *defs,
                                    uint32_t num_regs,
                                    uint64_t region_size)
{
    s->reg_defs = defs;
    s->num_regs = num_regs;
    s->region_size = region_size;
}

void beta_generic_mmio_add_callback(BetaGenericMMIOState *s,
                                    const char *reg_name,
                                    BetaMMIOWriteCallback fn,
                                    void *opaque)
{
    if (find_reg_by_name(s, reg_name) < 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: callback for unknown register '%s'\n",
                      s->device_name ? s->device_name : "beta-mmio",
                      reg_name);
    }

    s->num_callbacks++;
    s->callbacks = g_renew(BetaMMIOCallback, s->callbacks, s->num_callbacks);
    BetaMMIOCallback *cb = &s->callbacks[s->num_callbacks - 1];
    cb->reg_name = g_strdup(reg_name);
    cb->fn = fn;
    cb->opaque = opaque;
}

uint64_t beta_generic_mmio_get_reg(BetaGenericMMIOState *s,
                                   const char *reg_name)
{
    int idx = find_reg_by_name(s, reg_name);
    if (idx < 0) {
        return 0;
    }
    return s->reg_values[idx];
}

void beta_generic_mmio_set_reg(BetaGenericMMIOState *s,
                               const char *reg_name, uint64_t value)
{
    int idx = find_reg_by_name(s, reg_name);
    if (idx >= 0) {
        s->reg_values[idx] = value;
    }
}
