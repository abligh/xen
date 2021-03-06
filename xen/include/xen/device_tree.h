/*
 * Device Tree
 *
 * Copyright (C) 2012 Citrix Systems, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef __XEN_DEVICE_TREE_H__
#define __XEN_DEVICE_TREE_H__

#include <xen/types.h>

#define DEVICE_TREE_MAX_DEPTH 16

#define NR_MEM_BANKS 8
#define NR_MODULES 2

struct membank {
    paddr_t start;
    paddr_t size;
};

struct dt_mem_info {
    int nr_banks;
    struct membank bank[NR_MEM_BANKS];
};

struct dt_gic_info {
    paddr_t gic_dist_addr;
    paddr_t gic_cpu_addr;
    paddr_t gic_hyp_addr;
    paddr_t gic_vcpu_addr;
};

struct dt_mb_module {
    paddr_t start;
    paddr_t size;
    char cmdline[1024];
};

struct dt_module_info {
    int nr_mods;
    /* Module 0 is Xen itself, followed by the provided modules-proper */
    struct dt_mb_module module[NR_MODULES + 1];
};

struct dt_early_info {
    struct dt_mem_info mem;
    struct dt_gic_info gic;
    struct dt_module_info modules;
};

typedef int (*device_tree_node_func)(const void *fdt,
                                     int node, const char *name, int depth,
                                     u32 address_cells, u32 size_cells,
                                     void *data);

extern struct dt_early_info early_info;
extern void *device_tree_flattened;

size_t device_tree_early_init(const void *fdt);

void device_tree_get_reg(const u32 **cell, u32 address_cells, u32 size_cells,
                         u64 *start, u64 *size);
void device_tree_set_reg(u32 **cell, u32 address_cells, u32 size_cells,
                         u64 start, u64 size);
u32 device_tree_get_u32(const void *fdt, int node, const char *prop_name,
			u32 dflt);
bool_t device_tree_node_matches(const void *fdt, int node, const char *match);
bool_t device_tree_node_compatible(const void *fdt, int node, const char *match);
int find_compatible_node(const char *compatible, int *node, int *depth,
                u32 *address_cells, u32 *size_cells);
int device_tree_for_each_node(const void *fdt,
                              device_tree_node_func func, void *data);
const char *device_tree_bootargs(const void *fdt);
void device_tree_dump(const void *fdt);

#endif
