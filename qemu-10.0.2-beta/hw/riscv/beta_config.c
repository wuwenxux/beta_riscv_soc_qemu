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

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qobject/qjson.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qstring.h"
#include "qobject/qnum.h"
#include "qobject/qobject.h"
#include "beta_config.h"

/* --------------- helper utilities --------------- */

static uint64_t parse_hex(const char *str)
{
    if (!str) {
        return 0;
    }
    return strtoull(str, NULL, 0);
}

static char *get_str(QDict *d, const char *key)
{
    const char *s = qdict_get_try_str(d, key);
    return s ? g_strdup(s) : NULL;
}

static char *get_str_req(QDict *d, const char *key, const char *section,
                         Error **errp)
{
    const char *s = qdict_get_try_str(d, key);
    if (!s) {
        error_setg(errp, "beta_config: missing required string '%s' in %s",
                   key, section);
        return NULL;
    }
    return g_strdup(s);
}

static uint64_t get_hex(QDict *d, const char *key)
{
    const char *s = qdict_get_try_str(d, key);
    return parse_hex(s);
}



static int64_t get_int_or(QDict *d, const char *key, int64_t def)
{
    return qdict_get_try_int(d, key, def);
}

static bool get_bool_or(QDict *d, const char *key, bool def)
{
    return qdict_get_try_bool(d, key, def);
}

/* --------------- section parsers --------------- */

static bool parse_cache(QDict *d, BetaCacheConfig *cache)
{
    cache->l1i_size_kb   = get_int_or(d, "l1i_size_kb", 0);
    cache->l1i_ways      = get_int_or(d, "l1i_ways", 0);
    cache->l1d_size_kb   = get_int_or(d, "l1d_size_kb", 0);
    cache->l1d_ways      = get_int_or(d, "l1d_ways", 0);
    cache->l2_size_kb    = get_int_or(d, "l2_size_kb", 0);
    cache->l2_ways       = get_int_or(d, "l2_ways", 0);
    cache->l2_inclusive   = get_bool_or(d, "l2_inclusive", false);
    cache->l3_slice_kb   = get_int_or(d, "l3_slice_kb", 0);
    cache->l3_num_slices = get_int_or(d, "l3_num_slices", 0);
    cache->line_bytes    = get_int_or(d, "line_bytes", 64);
    return true;
}

static bool parse_acpu(QDict *d, BetaACPUConfig *acpu, Error **errp)
{
    acpu->num_clusters      = get_int_or(d, "num_clusters", 1);
    acpu->cores_per_cluster = get_int_or(d, "cores_per_cluster", 1);
    acpu->hartid_base       = get_int_or(d, "hartid_base", 0);

    acpu->isa = get_str_req(d, "isa", "acpu", errp);
    if (!acpu->isa) {
        return false;
    }
    if (!g_str_has_prefix(acpu->isa, "rv64")) {
        error_setg(errp, "beta_config: acpu.isa must start with 'rv64', got '%s'",
                   acpu->isa);
        return false;
    }

    acpu->mmu_mode     = get_str(d, "mmu_mode");
    acpu->pma_type     = get_str(d, "pma_type");
    acpu->reset_vector = get_hex(d, "reset_vector");

    if (qdict_haskey(d, "cache")) {
        QDict *cache_d = qdict_get_qdict(d, "cache");
        parse_cache(cache_d, &acpu->cache);
    }

    if (qdict_haskey(d, "pmu")) {
        QDict *pmu = qdict_get_qdict(d, "pmu");
        acpu->pmu_fixed_counters = get_int_or(pmu, "fixed_counters", 0);
        acpu->pmu_prog_counters  = get_int_or(pmu, "prog_counters", 0);
        acpu->pmu_sscofpmf       = get_bool_or(pmu, "sscofpmf", false);
    }

    /* BUG-P0-03: machine identification CSRs (default 0 = vendor/arch not assigned) */
    acpu->mvendorid = get_hex(d, "mvendorid");
    acpu->marchid   = get_hex(d, "marchid");
    acpu->mimpid    = get_hex(d, "mimpid");

    return true;
}

static bool parse_mcpu(QDict *d, BetaMCPUConfig *mcpu, Error **errp)
{
    mcpu->num_cores = get_int_or(d, "num_cores", 1);

    mcpu->isa = get_str_req(d, "isa", "mcpu", errp);
    if (!mcpu->isa) {
        return false;
    }
    if (!g_str_has_prefix(mcpu->isa, "rv32")) {
        error_setg(errp, "beta_config: mcpu.isa must start with 'rv32', got '%s'",
                   mcpu->isa);
        return false;
    }

    mcpu->itcm_kb      = get_int_or(d, "itcm_kb", 0);
    mcpu->dtcm_kb      = get_int_or(d, "dtcm_kb", 0);
    mcpu->sram_kb      = get_int_or(d, "sram_kb", 0);
    mcpu->icache_kb    = get_int_or(d, "icache_kb", 0);
    mcpu->icache_ways  = get_int_or(d, "icache_ways", 0);
    mcpu->reset_vector = get_hex(d, "reset_vector");
    mcpu->hartid_base  = get_int_or(d, "hartid_base", 0);  /* 0 = auto */
    return true;
}

static bool parse_memory_regions(QList *list, BetaMemRegion **out,
                                 uint32_t *count, Error **errp)
{
    uint32_t n = qlist_size(list);
    BetaMemRegion *regions = g_new0(BetaMemRegion, n);
    const QListEntry *entry;
    uint32_t i = 0;

    QLIST_FOREACH_ENTRY(list, entry) {
        QDict *rd = qobject_to(QDict, qlist_entry_obj(entry));
        if (!rd) {
            error_setg(errp, "beta_config: memory_regions[%u] is not an object",
                       i);
            g_free(regions);
            return false;
        }
        regions[i].name = get_str_req(rd, "name", "memory_regions", errp);
        if (!regions[i].name) {
            g_free(regions);
            return false;
        }
        regions[i].base        = get_hex(rd, "base");
        regions[i].size        = get_hex(rd, "size");
        regions[i].type        = get_str(rd, "type");
        regions[i].cpu_visible = get_str(rd, "cpu_visible");
        i++;
    }

    *out = regions;
    *count = n;
    return true;
}

static bool parse_reg_fields(QList *list, BetaRegFieldDef **out,
                             uint32_t *count)
{
    uint32_t n = qlist_size(list);
    BetaRegFieldDef *fields = g_new0(BetaRegFieldDef, n);
    const QListEntry *entry;
    uint32_t i = 0;

    QLIST_FOREACH_ENTRY(list, entry) {
        QDict *fd = qobject_to(QDict, qlist_entry_obj(entry));
        if (!fd) {
            g_free(fields);
            return false;
        }
        fields[i].name   = get_str(fd, "name");
        fields[i].hi_bit = get_int_or(fd, "hi_bit", 0);
        fields[i].lo_bit = get_int_or(fd, "lo_bit", 0);
        fields[i].access = get_str(fd, "access");
        fields[i].reset  = get_hex(fd, "reset");
        i++;
    }

    *out = fields;
    *count = n;
    return true;
}

static bool parse_registers(QList *list, BetaRegDef **out, uint32_t *count)
{
    uint32_t n = qlist_size(list);
    BetaRegDef *regs = g_new0(BetaRegDef, n);
    const QListEntry *entry;
    uint32_t i = 0;

    QLIST_FOREACH_ENTRY(list, entry) {
        QDict *rd = qobject_to(QDict, qlist_entry_obj(entry));
        if (!rd) {
            g_free(regs);
            return false;
        }
        regs[i].name        = get_str(rd, "name");
        regs[i].offset      = get_hex(rd, "offset");
        regs[i].width       = get_int_or(rd, "width", 32);
        regs[i].reset_value = get_hex(rd, "reset_value");
        regs[i].access      = get_str(rd, "access");

        if (qdict_haskey(rd, "fields")) {
            QList *fl = qdict_get_qlist(rd, "fields");
            if (!parse_reg_fields(fl, &regs[i].fields,
                                  &regs[i].num_fields)) {
                g_free(regs);
                return false;
            }
        }
        i++;
    }

    *out = regs;
    *count = n;
    return true;
}

static bool parse_irq_map(QList *list, BetaIRQEntry **out, uint32_t *count)
{
    uint32_t n = qlist_size(list);
    BetaIRQEntry *entries = g_new0(BetaIRQEntry, n);
    const QListEntry *entry;
    uint32_t i = 0;

    QLIST_FOREACH_ENTRY(list, entry) {
        QDict *id = qobject_to(QDict, qlist_entry_obj(entry));
        if (!id) {
            g_free(entries);
            return false;
        }
        entries[i].name    = get_str(id, "name");
        entries[i].irq_num = get_int_or(id, "irq_num", 0);
        entries[i].target  = get_str(id, "target");
        entries[i].domain  = get_str(id, "domain");
        i++;
    }

    *out = entries;
    *count = n;
    return true;
}

static bool parse_aplic_domains(QList *list, BetaAPLICDomain **out,
                                uint32_t *count)
{
    uint32_t n = qlist_size(list);
    BetaAPLICDomain *domains = g_new0(BetaAPLICDomain, n);
    const QListEntry *entry;
    uint32_t i = 0;

    QLIST_FOREACH_ENTRY(list, entry) {
        QDict *dd = qobject_to(QDict, qlist_entry_obj(entry));
        if (!dd) {
            g_free(domains);
            return false;
        }
        domains[i].name   = get_str(dd, "name");
        domains[i].level  = get_str(dd, "level");
        domains[i].base   = get_hex(dd, "base");
        domains[i].size   = get_hex(dd, "size");
        domains[i].parent = get_str(dd, "parent");
        i++;
    }

    *out = domains;
    *count = n;
    return true;
}

static bool parse_interrupts(QDict *d, BetaInterruptConfig *intr, Error **errp)
{
    /* APLIC */
    if (qdict_haskey(d, "aplic")) {
        QDict *aplic_d = qdict_get_qdict(d, "aplic");
        intr->aplic.num_sources = get_int_or(aplic_d, "num_sources", 0);

        if (qdict_haskey(aplic_d, "domains")) {
            QList *dl = qdict_get_qlist(aplic_d, "domains");
            if (!parse_aplic_domains(dl, &intr->aplic.domains,
                                     &intr->aplic.num_domains)) {
                error_setg(errp, "beta_config: failed to parse aplic domains");
                return false;
            }
        }
    }

    /* IMSIC */
    if (qdict_haskey(d, "imsic")) {
        QDict *im = qdict_get_qdict(d, "imsic");
        intr->imsic.m_base           = get_hex(im, "m_base");
        intr->imsic.s_base           = get_hex(im, "s_base");
        intr->imsic.guest_index_bits = get_int_or(im, "guest_index_bits", 0);
        intr->imsic.num_ids          = get_int_or(im, "num_ids", 0);
        intr->imsic.num_vs_files     = get_int_or(im, "num_vs_files", 0);
    }

    /* CLINT */
    if (qdict_haskey(d, "clint")) {
        QDict *cl = qdict_get_qdict(d, "clint");
        intr->clint.base          = get_hex(cl, "base");
        intr->clint.size          = get_hex(cl, "size");
        intr->clint.timebase_freq = get_int_or(cl, "timebase_freq", 10000000);
    }

    /* PLIC */
    if (qdict_haskey(d, "plic")) {
        QDict *pl = qdict_get_qdict(d, "plic");
        intr->plic.base           = get_hex(pl, "base");
        intr->plic.size           = get_hex(pl, "size");
        intr->plic.num_sources    = get_int_or(pl, "num_sources", 0);
        intr->plic.num_priorities = get_int_or(pl, "num_priorities", 0);
    }

    /* IRQ map */
    if (qdict_haskey(d, "irq_map")) {
        QList *il = qdict_get_qlist(d, "irq_map");
        if (!parse_irq_map(il, &intr->irq_map, &intr->num_irqs)) {
            error_setg(errp, "beta_config: failed to parse irq_map");
            return false;
        }
    }

    return true;
}

static bool parse_devices(QList *list, BetaDeviceConfig **out,
                          uint32_t *count, Error **errp)
{
    uint32_t n = qlist_size(list);
    BetaDeviceConfig *devs = g_new0(BetaDeviceConfig, n);
    const QListEntry *entry;
    uint32_t i = 0;

    QLIST_FOREACH_ENTRY(list, entry) {
        QDict *dd = qobject_to(QDict, qlist_entry_obj(entry));
        if (!dd) {
            error_setg(errp, "beta_config: devices[%u] is not an object", i);
            g_free(devs);
            return false;
        }

        devs[i].name = get_str_req(dd, "name", "devices", errp);
        if (!devs[i].name) {
            g_free(devs);
            return false;
        }

        devs[i].type        = get_str(dd, "type");
        devs[i].base        = get_hex(dd, "base");
        devs[i].size        = get_hex(dd, "size");
        devs[i].cpu_target  = get_str(dd, "cpu_target");
        devs[i].compatible  = get_str(dd, "compatible");
        devs[i].irq_name    = get_str(dd, "irq_name");
        devs[i].clock_hz    = get_int_or(dd, "clock_hz", 0);
        devs[i].instance_id = get_int_or(dd, "instance_id", -1);

        if (qdict_haskey(dd, "registers")) {
            QList *rl = qdict_get_qlist(dd, "registers");
            if (!parse_registers(rl, &devs[i].registers,
                                 &devs[i].num_registers)) {
                error_setg(errp,
                           "beta_config: failed to parse registers for %s",
                           devs[i].name);
                g_free(devs);
                return false;
            }
        }
        i++;
    }

    *out = devs;
    *count = n;
    return true;
}

static bool parse_ddr(QDict *d, BetaDDRConfig *ddr, Error **errp)
{
    ddr->num_channels         = get_int_or(d, "num_channels", 0);
    ddr->channel_width_bits   = get_int_or(d, "channel_width_bits", 64);
    ddr->speed_mbps           = get_int_or(d, "speed_mbps", 0);
    ddr->max_per_channel_gb   = get_int_or(d, "max_per_channel_gb", 0);
    ddr->interleave_step_bytes = get_int_or(d, "interleave_step_bytes", 0);

    if (qdict_haskey(d, "channels")) {
        QList *cl = qdict_get_qlist(d, "channels");
        uint32_t n = qlist_size(cl);
        ddr->channels = g_new0(BetaDDRChannel, n);
        const QListEntry *entry;
        uint32_t i = 0;

        QLIST_FOREACH_ENTRY(cl, entry) {
            QDict *cd = qobject_to(QDict, qlist_entry_obj(entry));
            if (!cd) {
                error_setg(errp,
                           "beta_config: ddr.channels[%u] is not an object",
                           i);
                return false;
            }
            ddr->channels[i].id        = get_int_or(cd, "id", i);
            ddr->channels[i].ctrl_base = get_hex(cd, "ctrl_base");
            ddr->channels[i].mem_base  = get_hex(cd, "mem_base");
            ddr->channels[i].size      = get_hex(cd, "size");
            i++;
        }
    }

    return true;
}

static bool parse_boot(QDict *d, BetaBootConfig *boot, Error **errp)
{
    boot->mode = get_str_req(d, "mode", "boot", errp);
    if (!boot->mode) {
        return false;
    }

    boot->bootrom_base = get_hex(d, "bootrom_base");
    boot->bootrom_size = get_hex(d, "bootrom_size");
    boot->flash_base   = get_hex(d, "flash_base");
    boot->flash_size   = get_hex(d, "flash_size");
    boot->mcpu_entry   = get_hex(d, "mcpu_entry");
    boot->acpu_entry   = get_hex(d, "acpu_entry");
    return true;
}

static bool parse_pcie(QList *list, BetaPCIeConfig **out,
                       uint32_t *count, Error **errp)
{
    uint32_t n = qlist_size(list);
    BetaPCIeConfig *pcie = g_new0(BetaPCIeConfig, n);
    const QListEntry *entry;
    uint32_t i = 0;

    QLIST_FOREACH_ENTRY(list, entry) {
        QDict *pd = qobject_to(QDict, qlist_entry_obj(entry));
        if (!pd) {
            error_setg(errp, "beta_config: pcie[%u] is not an object", i);
            g_free(pcie);
            return false;
        }
        pcie[i].dbi_base    = get_hex(pd, "dbi_base");
        pcie[i].dbi_size    = get_hex(pd, "dbi_size");
        pcie[i].ecam_base   = get_hex(pd, "ecam_base");
        pcie[i].ecam_size   = get_hex(pd, "ecam_size");
        pcie[i].mmio_base   = get_hex(pd, "mmio_base");
        pcie[i].mmio_size   = get_hex(pd, "mmio_size");
        pcie[i].mmio64_base = get_hex(pd, "mmio64_base");
        pcie[i].mmio64_size = get_hex(pd, "mmio64_size");
        pcie[i].num_lanes     = get_int_or(pd, "num_lanes", 1);
        pcie[i].bifurcation   = get_int_or(pd, "bifurcation", 1);
        pcie[i].irq_name      = get_str(pd, "irq_name");
        pcie[i].irq_intx_name = get_str(pd, "irq_intx_name");
        pcie[i].iommu_base     = get_hex(pd, "iommu_base");
        pcie[i].iommu_base_irq = get_int_or(pd, "iommu_base_irq", 0);
        i++;
    }

    *out = pcie;
    *count = n;
    return true;
}

/* --------------- post-load validation --------------- */

/*
 * Validate irq_map entries against aplic.num_sources:
 *  - irq_num must be in [1, num_sources] (APLIC source 0 is reserved)
 *  - irq_num values must be unique
 */
static bool validate_irq_map(BetaInterruptConfig *intr, Error **errp)
{
    uint32_t num_sources = intr->aplic.num_sources;
    uint32_t i, j;

    for (i = 0; i < intr->num_irqs; i++) {
        uint32_t irq = intr->irq_map[i].irq_num;
        const char *name = intr->irq_map[i].name ? intr->irq_map[i].name : "?";

        if (num_sources > 0 && (irq == 0 || irq > num_sources)) {
            error_setg(errp,
                       "beta_config: irq_map[%u] \"%s\" irq_num=%u out of "
                       "range [1, %u] (aplic.num_sources)",
                       i, name, irq, num_sources);
            return false;
        }

        for (j = 0; j < i; j++) {
            if (intr->irq_map[j].irq_num == irq) {
                error_setg(errp,
                           "beta_config: irq_map[%u] \"%s\" has duplicate "
                           "irq_num=%u (same as entry [%u] \"%s\")",
                           i, name, irq, j,
                           intr->irq_map[j].name ? intr->irq_map[j].name : "?");
                return false;
            }
        }
    }
    return true;
}

/* --------------- public API --------------- */

BetaSoCConfig *beta_config_load(const char *filename, Error **errp)
{
    g_autofree char *contents = NULL;
    gsize len;
    GError *gerr = NULL;
    QObject *obj = NULL;
    QDict *root = NULL;
    BetaSoCConfig *cfg = NULL;

    if (!g_file_get_contents(filename, &contents, &len, &gerr)) {
        error_setg(errp, "beta_config: cannot read '%s': %s",
                   filename, gerr->message);
        g_error_free(gerr);
        return NULL;
    }

    obj = qobject_from_json(contents, errp);
    if (!obj) {
        return NULL;
    }

    root = qobject_to(QDict, obj);
    if (!root) {
        error_setg(errp, "beta_config: top-level value is not a JSON object");
        qobject_unref(obj);
        return NULL;
    }

    cfg = g_new0(BetaSoCConfig, 1);

    /* version */
    cfg->version = get_str(root, "version");

    /* acpu */
    if (qdict_haskey(root, "acpu")) {
        QDict *acpu_d = qdict_get_qdict(root, "acpu");
        if (!parse_acpu(acpu_d, &cfg->acpu, errp)) {
            goto fail;
        }
    }

    /* mcpu */
    if (qdict_haskey(root, "mcpu")) {
        QDict *mcpu_d = qdict_get_qdict(root, "mcpu");
        if (!parse_mcpu(mcpu_d, &cfg->mcpu, errp)) {
            goto fail;
        }
    }

    /* memory_regions */
    if (qdict_haskey(root, "memory_regions")) {
        QList *mr = qdict_get_qlist(root, "memory_regions");
        if (!parse_memory_regions(mr, &cfg->memory_regions,
                                  &cfg->num_memory_regions, errp)) {
            goto fail;
        }
    }

    /* interrupts */
    if (qdict_haskey(root, "interrupts")) {
        QDict *intr_d = qdict_get_qdict(root, "interrupts");
        if (!parse_interrupts(intr_d, &cfg->interrupts, errp)) {
            goto fail;
        }
        if (!validate_irq_map(&cfg->interrupts, errp)) {
            goto fail;
        }
    }

    /* devices */
    if (qdict_haskey(root, "devices")) {
        QList *dl = qdict_get_qlist(root, "devices");
        if (!parse_devices(dl, &cfg->devices, &cfg->num_devices, errp)) {
            goto fail;
        }
    }

    /* ddr */
    if (qdict_haskey(root, "ddr")) {
        QDict *ddr_d = qdict_get_qdict(root, "ddr");
        if (!parse_ddr(ddr_d, &cfg->ddr, errp)) {
            goto fail;
        }
    }

    /* pcie */
    if (qdict_haskey(root, "pcie")) {
        QList *pl = qdict_get_qlist(root, "pcie");
        if (!parse_pcie(pl, &cfg->pcie, &cfg->num_pcie, errp)) {
            goto fail;
        }
    }

    /* boot */
    if (qdict_haskey(root, "boot")) {
        QDict *boot_d = qdict_get_qdict(root, "boot");
        if (!parse_boot(boot_d, &cfg->boot, errp)) {
            goto fail;
        }
    }

    qobject_unref(obj);
    return cfg;

fail:
    beta_config_free(cfg);
    qobject_unref(obj);
    return NULL;
}

void beta_config_free(BetaSoCConfig *config)
{
    uint32_t i, j;

    if (!config) {
        return;
    }

    g_free(config->version);

    /* acpu */
    g_free(config->acpu.isa);
    g_free(config->acpu.mmu_mode);
    g_free(config->acpu.pma_type);

    /* mcpu */
    g_free(config->mcpu.isa);

    /* memory_regions */
    for (i = 0; i < config->num_memory_regions; i++) {
        g_free(config->memory_regions[i].name);
        g_free(config->memory_regions[i].type);
        g_free(config->memory_regions[i].cpu_visible);
    }
    g_free(config->memory_regions);

    /* interrupts - aplic domains */
    for (i = 0; i < config->interrupts.aplic.num_domains; i++) {
        g_free(config->interrupts.aplic.domains[i].name);
        g_free(config->interrupts.aplic.domains[i].level);
        g_free(config->interrupts.aplic.domains[i].parent);
    }
    g_free(config->interrupts.aplic.domains);

    /* interrupts - irq_map */
    for (i = 0; i < config->interrupts.num_irqs; i++) {
        g_free(config->interrupts.irq_map[i].name);
        g_free(config->interrupts.irq_map[i].target);
        g_free(config->interrupts.irq_map[i].domain);
    }
    g_free(config->interrupts.irq_map);

    /* devices */
    for (i = 0; i < config->num_devices; i++) {
        BetaDeviceConfig *dev = &config->devices[i];
        g_free(dev->name);
        g_free(dev->type);
        g_free(dev->cpu_target);
        g_free(dev->compatible);
        g_free(dev->irq_name);

        for (j = 0; j < dev->num_registers; j++) {
            BetaRegDef *reg = &dev->registers[j];
            g_free(reg->name);
            g_free(reg->access);

            for (uint32_t k = 0; k < reg->num_fields; k++) {
                g_free(reg->fields[k].name);
                g_free(reg->fields[k].access);
            }
            g_free(reg->fields);
        }
        g_free(dev->registers);
    }
    g_free(config->devices);

    /* pcie */
    for (i = 0; i < config->num_pcie; i++) {
        g_free(config->pcie[i].irq_name);
        g_free(config->pcie[i].irq_intx_name);
    }
    g_free(config->pcie);

    /* ddr */
    g_free(config->ddr.channels);

    /* boot */
    g_free(config->boot.mode);

    g_free(config);
}
