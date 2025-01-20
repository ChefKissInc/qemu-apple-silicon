/*
 * General Apple XNU memory utilities.
 *
 * Copyright (c) 2023-2025 Visual Ehrmanntraut (VisualEhrmanntraut).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "exec/memory.h"
#include "hw/arm/apple-silicon/mem.h"
#include "qapi/error.h"

hwaddr g_virt_base, g_phys_base, g_virt_slide, g_phys_slide;

hwaddr vtop_bases(hwaddr va, hwaddr phys_base, hwaddr virt_base)
{
    g_assert_cmphex(phys_base, !=, 0);
    g_assert_cmphex(virt_base, !=, 0);

    return va - virt_base + phys_base;
}

hwaddr ptov_bases(hwaddr pa, hwaddr phys_base, hwaddr virt_base)
{
    g_assert_cmphex(phys_base, !=, 0);
    g_assert_cmphex(virt_base, !=, 0);

    return pa - phys_base + virt_base;
}

hwaddr vtop_static(hwaddr va)
{
    return vtop_bases(va, g_phys_base, g_virt_base);
}

hwaddr ptov_static(hwaddr pa)
{
    return ptov_bases(pa, g_phys_base, g_virt_base);
}

hwaddr vtop_slid(hwaddr va)
{
    return vtop_static(va + g_virt_slide);
}

MemoryRegion *allocate_ram(MemoryRegion *top, const char *name, hwaddr addr,
                           hwaddr size, int priority)
{
    MemoryRegion *sec = g_new(MemoryRegion, 1);
    g_assert_nonnull(sec);
    memory_region_init_ram(sec, NULL, name, size, &error_fatal);
    memory_region_add_subregion_overlap(top, addr, sec, priority);
    return sec;
}
