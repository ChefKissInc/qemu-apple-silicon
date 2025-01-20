/*
 * Apple Display Pipe V2 Controller.
 *
 * Copyright (c) 2023-2025 Visual Ehrmanntraut.
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
#ifndef APPLE_DISPLAYPIPE_V2_H
#define APPLE_DISPLAYPIPE_V2_H

#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "hw/sysbus.h"
#include "qom/object.h"
#include "ui/console.h"

#define TYPE_APPLE_DISPLAYPIPE_V2 "apple-displaypipe-v2"
OBJECT_DECLARE_SIMPLE_TYPE(AppleDisplayPipeV2State, APPLE_DISPLAYPIPE_V2);

typedef struct {
    uint32_t start;
    uint32_t end;
    uint32_t stride;
    uint32_t size;
} GenPipeLayer;

typedef struct {
    size_t index;
    MemoryRegion *vram;
    AddressSpace *dma_as;
    QEMUBH *bh;
    // TODO: Not have this field.
    AppleDisplayPipeV2State *disp_state;
    uint32_t config_control;
    uint32_t pixel_format;
    uint16_t width;
    uint16_t height;
    GenPipeLayer layers[2];
} GenPipeState;

struct AppleDisplayPipeV2State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    uint32_t width, height;
    MemoryRegion up_regs, vram;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    MemoryRegionSection vram_section;
    qemu_irq irqs[9];
    uint32_t int_filter;
    GenPipeState genpipes[2];
    QemuConsole *console;
    bool invalidated;
};

AppleDisplayPipeV2State *apple_displaypipe_v2_create(DTBNode *node);

#endif /* APPLE_DISPLAYPIPE_V2_H */
