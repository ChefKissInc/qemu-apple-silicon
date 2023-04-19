/*
 * AppleMobileDispH12P
 *
 * Copyright (c) 2023 ChefKiss Inc. Licensed under the Thou Shalt Not Profit License version 1.0. See LICENSE.TSNPL
 * for details.
 *
 */
#ifndef HW_FB_APPLE_H12P_H
#define HW_FB_APPLE_H12P_H

#include "sysemu/dma.h"
#include "ui/console.h"
#include "qom/object.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "qemu/timer.h"

#define T8030_DISPLAY_BASE (0x8F7FB4000)
#define T8030_DISPLAY_SIZE (35 * 1024 * 1024)

#define TYPE_APPLE_H12P "apple-h12p"
OBJECT_DECLARE_SIMPLE_TYPE(AppleH12PState, APPLE_H12P);

#define REG_UPPIPE_VER                (0x46020)
#define REG_UPPIPE_FRAME_SIZE         (0x4603C)
#define REG_GENPIPE0_BLACK_FRAME      (0x50004)
#define REG_GENPIPE1_BLACK_FRAME      (0x58004)
#define REG_GENPIPE0_PIXEL_FORMAT     (0x5001C)
#define REG_GENPIPE1_PIXEL_FORMAT     (0x5801C)
#define GENPIPE_DFB_PIXEL_FORMAT_BGRA (0x11110)
#define REG_GENPIPE0_PLANE_START      (0x50030)
#define REG_GENPIPE0_PLANE_END        (0x50040)
#define REG_GENPIPE1_PLANE_START      (0x58030)
#define REG_GENPIPE1_PLANE_END        (0x58040)
#define REG_GENPIPE0_FRAME_SIZE       (0x50080)
#define REG_GENPIPE1_FRAME_SIZE       (0x58080)
#define REG_UP_CONFIG_FRAME_SIZE      (0x24603C)

struct AppleH12PState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion up_regs, vram;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    MemoryRegionSection vram_section;
    QemuConsole *console;

    /* Configuration data for the FB */
    uint32_t width, height;

    uint32_t genpipe0_plane_start, genpipe0_plane_end;
    uint8_t regs[0xBC2D000];
};

void apple_h12p_create(MachineState *machine);

#endif /* HW_FB_APPLE_H12P_H */