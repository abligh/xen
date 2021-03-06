/*
 * Device Tree
 *
 * Copyright (C) 2012 Citrix Systems, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <xen/config.h>
#include <xen/types.h>
#include <xen/init.h>
#include <xen/device_tree.h>
#include <xen/kernel.h>
#include <xen/lib.h>
#include <xen/libfdt/libfdt.h>
#include <xen/mm.h>
#include <xen/stdarg.h>
#include <xen/string.h>
#include <xen/cpumask.h>
#include <asm/early_printk.h>

struct dt_early_info __initdata early_info;
void *device_tree_flattened;

/* Some device tree functions may be called both before and after the
   console is initialized. */
static void (*dt_printk)(const char *fmt, ...) = early_printk;

bool_t device_tree_node_matches(const void *fdt, int node, const char *match)
{
    const char *name;
    size_t match_len;

    name = fdt_get_name(fdt, node, NULL);
    match_len = strlen(match);

    /* Match both "match" and "match@..." patterns but not
       "match-foo". */
    return strncmp(name, match, match_len) == 0
        && (name[match_len] == '@' || name[match_len] == '\0');
}

bool_t device_tree_type_matches(const void *fdt, int node, const char *match)
{
    const void *prop;

    prop = fdt_getprop(fdt, node, "device_type", NULL);
    if ( prop == NULL )
        return 0;

    return !strcmp(prop, match);
}

bool_t device_tree_node_compatible(const void *fdt, int node, const char *match)
{
    int len, l;
    const void *prop;

    prop = fdt_getprop(fdt, node, "compatible", &len);
    if ( prop == NULL )
        return 0;

    while ( len > 0 ) {
        if ( !strcmp(prop, match) )
            return 1;
        l = strlen(prop) + 1;
        prop += l;
        len -= l;
    }

    return 0;
}

static int device_tree_nr_reg_ranges(const struct fdt_property *prop,
        u32 address_cells, u32 size_cells)
{
    u32 reg_cells = address_cells + size_cells;
    return fdt32_to_cpu(prop->len) / (reg_cells * sizeof(u32));
}

static void __init get_val(const u32 **cell, u32 cells, u64 *val)
{
    *val = 0;

    if ( cells > 2 )
        early_panic("dtb value contains > 2 cells\n");

    while ( cells-- )
    {
        *val <<= 32;
        *val |= fdt32_to_cpu(*(*cell)++);
    }
}

void device_tree_get_reg(const u32 **cell, u32 address_cells, u32 size_cells,
                         u64 *start, u64 *size)
{
    get_val(cell, address_cells, start);
    get_val(cell, size_cells, size);
}

static void set_val(u32 **cell, u32 cells, u64 val)
{
    u32 c = cells;

    while ( c-- )
    {
        (*cell)[c] = cpu_to_fdt32(val);
        val >>= 32;
    }
    (*cell) += cells;
}

void device_tree_set_reg(u32 **cell, u32 address_cells, u32 size_cells,
                         u64 start, u64 size)
{
    set_val(cell, address_cells, start);
    set_val(cell, size_cells, size);
}

u32 device_tree_get_u32(const void *fdt, int node, const char *prop_name,
                        u32 dflt)
{
    const struct fdt_property *prop;

    prop = fdt_get_property(fdt, node, prop_name, NULL);
    if ( !prop || prop->len < sizeof(u32) )
        return dflt;

    return fdt32_to_cpu(*(uint32_t*)prop->data);
}

/**
 * device_tree_for_each_node - iterate over all device tree nodes
 * @fdt: flat device tree.
 * @func: function to call for each node.
 * @data: data to pass to @func.
 *
 * Any nodes nested at DEVICE_TREE_MAX_DEPTH or deeper are ignored.
 *
 * Returns 0 if all nodes were iterated over successfully.  If @func
 * returns a value different from 0, that value is returned immediately.
 */
int device_tree_for_each_node(const void *fdt,
                              device_tree_node_func func, void *data)
{
    int node;
    int depth;
    u32 address_cells[DEVICE_TREE_MAX_DEPTH];
    u32 size_cells[DEVICE_TREE_MAX_DEPTH];
    int ret;

    for ( node = 0, depth = 0;
          node >=0 && depth >= 0;
          node = fdt_next_node(fdt, node, &depth) )
    {
        const char *name = fdt_get_name(fdt, node, NULL);

        if ( depth >= DEVICE_TREE_MAX_DEPTH )
        {
            dt_printk("Warning: device tree node `%s' is nested too deep\n",
                      name);
            continue;
        }

        address_cells[depth] = device_tree_get_u32(fdt, node, "#address-cells",
                                depth > 0 ? address_cells[depth-1] : 0);
        size_cells[depth] = device_tree_get_u32(fdt, node, "#size-cells",
                                depth > 0 ? size_cells[depth-1] : 0);


        ret = func(fdt, node, name, depth,
                   address_cells[depth-1], size_cells[depth-1], data);
        if ( ret != 0 )
            return ret;
    }
    return 0;
}

struct find_compat {
    const char *compatible;
    int found;
    int node;
    int depth;
    u32 address_cells;
    u32 size_cells;
};

static int _find_compatible_node(const void *fdt,
                             int node, const char *name, int depth,
                             u32 address_cells, u32 size_cells,
                             void *data)
{
    struct find_compat *c = (struct find_compat *) data;

    if (  c->found  )
        return 1;

    if ( device_tree_node_compatible(fdt, node, c->compatible) )
    {
        c->found = 1;
        c->node = node;
        c->depth = depth;
        c->address_cells = address_cells;
        c->size_cells = size_cells;
        return 1;
    }
    return 0;
}

int find_compatible_node(const char *compatible, int *node, int *depth,
                u32 *address_cells, u32 *size_cells)
{
    int ret;
    struct find_compat c;
    c.compatible = compatible;
    c.found = 0;

    ret = device_tree_for_each_node(device_tree_flattened, _find_compatible_node, &c);
    if ( !c.found )
        return ret;
    else
    {
        *node = c.node;
        *depth = c.depth;
        *address_cells = c.address_cells;
        *size_cells = c.size_cells;
        return 1;
    }
}

/**
 * device_tree_bootargs - return the bootargs (the Xen command line)
 * @fdt flat device tree.
 */
const char *device_tree_bootargs(const void *fdt)
{
    int node; 
    const struct fdt_property *prop;

    node = fdt_path_offset(fdt, "/chosen");
    if ( node < 0 )
        return NULL;

    prop = fdt_get_property(fdt, node, "bootargs", NULL);
    if ( prop == NULL )
        return NULL;

    return prop->data;
}

static int dump_node(const void *fdt, int node, const char *name, int depth,
                     u32 address_cells, u32 size_cells, void *data)
{
    char prefix[2*DEVICE_TREE_MAX_DEPTH + 1] = "";
    int i;
    int prop;

    for ( i = 0; i < depth; i++ )
        safe_strcat(prefix, "  ");

    if ( name[0] == '\0' )
        name = "/";
    printk("%s%s:\n", prefix, name);

    for ( prop = fdt_first_property_offset(fdt, node);
          prop >= 0;
          prop = fdt_next_property_offset(fdt, prop) )
    {
        const struct fdt_property *p;

        p = fdt_get_property_by_offset(fdt, prop, NULL);

        printk("%s  %s\n", prefix, fdt_string(fdt, fdt32_to_cpu(p->nameoff)));
    }

    return 0;
}

/**
 * device_tree_dump - print a text representation of a device tree
 * @fdt: flat device tree to print
 */
void device_tree_dump(const void *fdt)
{
    device_tree_for_each_node(fdt, dump_node, NULL);
}


static void __init process_memory_node(const void *fdt, int node,
                                       const char *name,
                                       u32 address_cells, u32 size_cells)
{
    const struct fdt_property *prop;
    int i;
    int banks;
    const u32 *cell;
    paddr_t start, size;

    if ( address_cells < 1 || size_cells < 1 )
    {
        early_printk("fdt: node `%s': invalid #address-cells or #size-cells",
                     name);
        return;
    }

    prop = fdt_get_property(fdt, node, "reg", NULL);
    if ( !prop )
    {
        early_printk("fdt: node `%s': missing `reg' property\n", name);
        return;
    }

    cell = (const u32 *)prop->data;
    banks = device_tree_nr_reg_ranges(prop, address_cells, size_cells);

    for ( i = 0; i < banks && early_info.mem.nr_banks < NR_MEM_BANKS; i++ )
    {
        device_tree_get_reg(&cell, address_cells, size_cells, &start, &size);
        early_info.mem.bank[early_info.mem.nr_banks].start = start;
        early_info.mem.bank[early_info.mem.nr_banks].size = size;
        early_info.mem.nr_banks++;
    }
}

static void __init process_cpu_node(const void *fdt, int node,
                                    const char *name,
                                    u32 address_cells, u32 size_cells)
{
    const struct fdt_property *prop;
    const u32 *cell;
    paddr_t start, size;

    if ( address_cells != 1 || size_cells != 0 )
    {
        early_printk("fdt: node `%s': invalid #address-cells or #size-cells",
                     name);
        return;
    }

    prop = fdt_get_property(fdt, node, "reg", NULL);
    if ( !prop )
    {
        early_printk("fdt: node `%s': missing `reg' property\n", name);
        return;
    }

    cell = (const u32 *)prop->data;
    device_tree_get_reg(&cell, address_cells, size_cells, &start, &size);

    cpumask_set_cpu(start, &cpu_possible_map);
}

static void __init process_gic_node(const void *fdt, int node,
                                    const char *name,
                                    u32 address_cells, u32 size_cells)
{
    const struct fdt_property *prop;
    const u32 *cell;
    paddr_t start, size;
    int interfaces;

    if ( address_cells < 1 || size_cells < 1 )
    {
        early_printk("fdt: node `%s': invalid #address-cells or #size-cells",
                     name);
        return;
    }

    prop = fdt_get_property(fdt, node, "reg", NULL);
    if ( !prop )
    {
        early_printk("fdt: node `%s': missing `reg' property\n", name);
        return;
    }

    cell = (const u32 *)prop->data;
    interfaces = device_tree_nr_reg_ranges(prop, address_cells, size_cells);
    if ( interfaces < 4 )
    {
        early_printk("fdt: node `%s': not enough ranges\n", name);
        return;
    }
    device_tree_get_reg(&cell, address_cells, size_cells, &start, &size);
    early_info.gic.gic_dist_addr = start;
    device_tree_get_reg(&cell, address_cells, size_cells, &start, &size);
    early_info.gic.gic_cpu_addr = start;
    device_tree_get_reg(&cell, address_cells, size_cells, &start, &size);
    early_info.gic.gic_hyp_addr = start;
    device_tree_get_reg(&cell, address_cells, size_cells, &start, &size);
    early_info.gic.gic_vcpu_addr = start;
}

static void __init process_multiboot_node(const void *fdt, int node,
                                          const char *name,
                                          u32 address_cells, u32 size_cells)
{
    const struct fdt_property *prop;
    const u32 *cell;
    int nr;
    struct dt_mb_module *mod;
    int len;

    if ( fdt_node_check_compatible(fdt, node, "xen,linux-zimage") == 0 )
        nr = 1;
    else if ( fdt_node_check_compatible(fdt, node, "xen,linux-initrd") == 0)
        nr = 2;
    else
        early_panic("%s not a known xen multiboot type\n", name);

    mod = &early_info.modules.module[nr];

    prop = fdt_get_property(fdt, node, "reg", NULL);
    if ( !prop )
        early_panic("node %s missing `reg' property\n", name);

    cell = (const u32 *)prop->data;
    device_tree_get_reg(&cell, address_cells, size_cells,
                        &mod->start, &mod->size);

    prop = fdt_get_property(fdt, node, "bootargs", &len);
    if ( prop )
    {
        if ( len > sizeof(mod->cmdline) )
            early_panic("module %d command line too long\n", nr);

        safe_strcpy(mod->cmdline, prop->data);
    }
    else
        mod->cmdline[0] = 0;

    if ( nr > early_info.modules.nr_mods )
        early_info.modules.nr_mods = nr;
}

static int __init early_scan_node(const void *fdt,
                                  int node, const char *name, int depth,
                                  u32 address_cells, u32 size_cells,
                                  void *data)
{
    if ( device_tree_node_matches(fdt, node, "memory") )
        process_memory_node(fdt, node, name, address_cells, size_cells);
    else if ( device_tree_type_matches(fdt, node, "cpu") )
        process_cpu_node(fdt, node, name, address_cells, size_cells);
    else if ( device_tree_node_compatible(fdt, node, "arm,cortex-a15-gic") )
        process_gic_node(fdt, node, name, address_cells, size_cells);
    else if ( device_tree_node_compatible(fdt, node, "xen,multiboot-module" ) )
        process_multiboot_node(fdt, node, name, address_cells, size_cells);

    return 0;
}

static void __init early_print_info(void)
{
    struct dt_mem_info *mi = &early_info.mem;
    struct dt_module_info *mods = &early_info.modules;
    int i;

    for ( i = 0; i < mi->nr_banks; i++ )
        early_printk("RAM: %"PRIpaddr" - %"PRIpaddr"\n",
                     mi->bank[i].start,
                     mi->bank[i].start + mi->bank[i].size - 1);
    early_printk("\n");
    for ( i = 1 ; i < mods->nr_mods + 1; i++ )
        early_printk("MODULE[%d]: %"PRIpaddr" - %"PRIpaddr" %s\n",
                     i,
                     mods->module[i].start,
                     mods->module[i].start + mods->module[i].size,
                     mods->module[i].cmdline);
}

/**
 * device_tree_early_init - initialize early info from a DTB
 * @fdt: flattened device tree binary
 *
 * Returns the size of the DTB.
 */
size_t __init device_tree_early_init(const void *fdt)
{
    int ret;

    ret = fdt_check_header(fdt);
    if ( ret < 0 )
        early_panic("No valid device tree\n");

    device_tree_for_each_node((void *)fdt, early_scan_node, NULL);
    early_print_info();

    dt_printk = printk;

    return fdt_totalsize(fdt);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
