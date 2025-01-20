/*
 * Apple Display Pipe V2 Controller.
 *
 * Copyright (c) 2023-2025 Visual Ehrmanntraut (VisualEhrmanntraut).
 * Copyright (c) 2023-2025 Christian Inci (chris-pcguy).
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
#include "block/aio.h"
#include "exec/memory.h"
#include "hw/display/apple_displaypipe_v2.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "sysemu/dma.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "framebuffer.h"

// #define DEBUG_DISP

#ifdef DEBUG_DISP
#define DISP_DBGLOG(fmt, ...) \
    qemu_log_mask(LOG_GUEST_ERROR, fmt "\n", __VA_ARGS__)
#else
#define DISP_DBGLOG(fmt, ...) \
    do {                      \
    } while (0);
#endif

/**
 * Block bases
 * 0x40000  |  Control
 * 0x48000  |  Vertical Frame Timing Generator
 * 0x50000  |  Generic Pipe 0
 * 0x58000  |  Generic Pipe 1
 * 0x70000  |  White Point Correction
 * 0x7C000  |  Panel Response Correction
 * 0x80000  |  Dither
 * 0x82000  |  Dither: Enchanced ST Dither 0
 * 0x83000  |  Dither: Enchanced ST Dither 1
 * 0x84000  |  Content Dependent Frame Duration
 * 0x88000  |  SPLR (Sub-Pixel Layout R?)
 * 0x90000  |  Burn-In Compensation Sampler
 * 0xA0000  |  PDC
 * 0xB0000  |  PCC
 * 0xF0000  |  DBM
 */

#define REG_CONTROL_INT_FILTER 0x45818
#define REG_CONTROL_VERSION 0x46020
#define CONTROL_VERSION_A0 0x70044
#define CONTROL_VERSION_A1 0x70045
#define REG_CONTROL_FRAME_SIZE 0x4603C
#define REG_CONTROL_CONFIG 0x46040
#define REG_CONTROL_OUT_FIFO_CLK_GATE 0x46074
#define REG_CONTROL_OUT_FIFO_DEPTH 0x46084
#define REG_CONTROL_COMPRESSION_CFG 0x460E0
#define REG_CONTROL_BACKPRESSURE 0x46120
#define REG_CONTROL_POWER_GATE_CTRL 0x46158
#define REG_CONTROL_BIS_UPDATE_INTERVAL 0x46198
#define REG_CONTROL_MIN_BANDWIDTH_RATE 0x461C0
#define REG_CONTROL_BANDWIDTH_RATE_SCALE_FACTOR 0x461C4
#define REG_CONTROL_PIO_DMA_BANDWIDTH_RATE 0x461C8
#define REG_CONTROL_REPLAY_DMA_BANDWIDTH_RATE 0x461CC
#define REG_CONTROL_GATE_CONTROL 0x461D0
#define REG_CONTROL_READ_LINK_GATE_METRIC 0x461D4
#define REG_CONTROL_READ_LTR_CONFIG 0x461D8
#define REG_CONTROL_LTR_TIMER 0x461DC
#define REG_CONTROL_WRITE_LTR_CONFIG 0x461E0

#define GP_BLOCK_BASE 0x50000
#define REG_GP_REG_SIZE 0x08000
#define REG_GP_CONFIG_CONTROL 0x00004
#define GP_CONFIG_CONTROL_RUN BIT(0)
#define GP_CONFIG_CONTROL_USE_DMA BIT(18)
#define GP_CONFIG_CONTROL_HDR BIT(24)
#define GP_CONFIG_CONTROL_ENABLED BIT(31)
#define REG_GP_PIXEL_FORMAT 0x0001C
#define GP_PIXEL_FORMAT_BGRA ((BIT(4) << 22) | BIT(24) | BIT(13))
#define GP_PIXEL_FORMAT_BGRA_MASK ((BIT(4) << 22) | BIT(24) | 3 << 13)
#define GP_PIXEL_FORMAT_ARGB ((BIT(4) << 22) | BIT(24))
#define GP_PIXEL_FORMAT_COMPRESSED BIT(30)
#define REG_GP_LAYER_0_START 0x00030
#define REG_GP_LAYER_1_START 0x00034
#define REG_GP_LAYER_0_END 0x00040
#define REG_GP_LAYER_1_END 0x00044
#define REG_GP_LAYER_0_STRIDE 0x00060
#define REG_GP_LAYER_1_STRIDE 0x00064
#define REG_GP_LAYER_0_SIZE 0x00070
#define REG_GP_LAYER_1_SIZE 0x00074
#define REG_GP_FRAME_SIZE 0x00080
#define REG_GP_CRC 0x00160
#define REG_GP_BANDWIDTH_RATE 0x00170
#define REG_GP_STATUS 0x00184
#define GP_STATUS_DECOMPRESSION_FAIL BIT(0)

#define GP_BLOCK_BASE_FOR(i) (GP_BLOCK_BASE + i * REG_GP_REG_SIZE)
#define GP_BLOCK_END_FOR(i) (GP_BLOCK_BASE_FOR(i) + (REG_GP_REG_SIZE - 1))

static void apple_disp_gp_reg_write(GenPipeState *s, hwaddr addr, uint64_t data)
{
    switch (addr - GP_BLOCK_BASE_FOR(s->index)) {
    case REG_GP_CONFIG_CONTROL: {
        DISP_DBGLOG("[GP%zu] Control <- 0x" HWADDR_FMT_plx, s->index, data);
        s->config_control = (uint32_t)data;
        if (data & GP_CONFIG_CONTROL_RUN) {
            qemu_bh_schedule(s->bh);
        }
        break;
    }
    case REG_GP_PIXEL_FORMAT: {
        DISP_DBGLOG("[GP%zu] Pixel format <- 0x" HWADDR_FMT_plx, s->index,
                    data);
        s->pixel_format = (uint32_t)data;
        break;
    }
    case REG_GP_LAYER_0_START: {
        DISP_DBGLOG("[GP%zu] Layer 0 start <- 0x" HWADDR_FMT_plx, s->index,
                    data);
        s->layers[0].start = (uint32_t)data;
        break;
    }
    case REG_GP_LAYER_0_END: {
        DISP_DBGLOG("[GP%zu] Layer 0 end <- 0x" HWADDR_FMT_plx, s->index, data);
        s->layers[0].end = (uint32_t)data;
        break;
    }
    case REG_GP_LAYER_0_STRIDE: {
        DISP_DBGLOG("[GP%zu] Layer 0 stride <- 0x" HWADDR_FMT_plx, s->index,
                    data);
        s->layers[0].stride = (uint32_t)data;
        break;
    }
    case REG_GP_LAYER_0_SIZE: {
        DISP_DBGLOG("[GP%zu] Layer 0 size <- 0x" HWADDR_FMT_plx, s->index,
                    data);
        s->layers[0].size = (uint32_t)data;
        break;
    }
    case REG_GP_FRAME_SIZE: {
        DISP_DBGLOG("[GP%zu] Frame size <- 0x" HWADDR_FMT_plx, s->index, data);
        s->height = data & 0xFFFF;
        s->width = (data >> 16) & 0xFFFF;
        break;
    }
    default: {
        DISP_DBGLOG("[GP%zu] Unknown write @ 0x" HWADDR_FMT_plx
                    " value: 0x" HWADDR_FMT_plx,
                    s->index, addr, data);
        break;
    }
    }
}

static uint32_t apple_disp_gp_reg_read(GenPipeState *s, hwaddr addr)
{
    switch (addr - GP_BLOCK_BASE_FOR(s->index)) {
    case REG_GP_CONFIG_CONTROL: {
        DISP_DBGLOG("[GP%zu] Control -> 0x%x", s->index, s->config_control);
        return s->config_control;
    }
    case REG_GP_PIXEL_FORMAT: {
        DISP_DBGLOG("[GP%zu] Pixel format -> 0x%x", s->index, s->pixel_format);
        return s->pixel_format;
    }
    case REG_GP_LAYER_0_START: {
        DISP_DBGLOG("[GP%zu] Layer 0 start -> 0x%x", s->index,
                    s->layers[0].start);
        return s->layers[0].start;
    }
    case REG_GP_LAYER_0_END: {
        DISP_DBGLOG("[GP%zu] Layer 0 end -> 0x%x", s->index, s->layers[0].end);
        return s->layers[0].end;
    }
    case REG_GP_LAYER_0_STRIDE: {
        DISP_DBGLOG("[GP%zu] Layer 0 stride -> 0x%x", s->index,
                    s->layers[0].stride);
        return s->layers[0].stride;
    }
    case REG_GP_LAYER_0_SIZE: {
        DISP_DBGLOG("[GP%zu] Layer 0 size -> 0x%x", s->index,
                    s->layers[0].size);
        return s->layers[0].size;
    }
    case REG_GP_FRAME_SIZE: {
        DISP_DBGLOG("[GP%zu] Frame size -> 0x%x (width: %d height: %d)",
                    s->index, (s->width << 16) | s->height, s->width,
                    s->height);
        return (s->width << 16) | s->height;
    }
    default: {
        DISP_DBGLOG("[GP%zu] Unknown read @ 0x" HWADDR_FMT_plx, s->index, addr);
        return 0;
    }
    }
}

static uint8_t *apple_disp_gp_read_layer(GenPipeState *s, size_t i,
                                         AddressSpace *dma_as, size_t *size_out)
{
    size_t size;
    uint8_t *buf;

    *size_out = 0;

    if (!s->layers[i].start || !s->layers[i].end) {
        return NULL;
    }

    size = s->layers[i].end - s->layers[i].start;
    buf = g_malloc(size);

    if (dma_memory_read(dma_as, s->layers[i].start, buf, size,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        g_free(buf);
        return NULL;
    }

    *size_out = size;
    return buf;
}

static void apple_gp_draw_bh(void *opaque)
{
    GenPipeState *s;
    size_t size;
    uint8_t *buf;

    s = (GenPipeState *)opaque;
    size = 0;
    buf = apple_disp_gp_read_layer(s, 0, s->dma_as, &size);

    if (buf == NULL) {
        return;
    }

    // TODO: Blend both layers. 2nd layer is currently not used.
    uint16_t height = s->layers[0].size & 0xFFFF;
    uint16_t width = (s->layers[0].size >> 16) & 0xFFFF;
    DISP_DBGLOG("[GP%zu] Layer 0 width and height is %dx%d.", s->index, width,
                height);
    DISP_DBGLOG("[GP%zu] Layer 0 stride is %d.", s->index, s->layers[0].stride);
    if ((s->pixel_format & GP_PIXEL_FORMAT_BGRA_MASK) ==
        GP_PIXEL_FORMAT_BGRA_MASK) {
        DISP_DBGLOG("[GP%zu] Pixel Format is BGRA (0x%X).", s->index,
                    s->pixel_format);
    } else if ((s->pixel_format & GP_PIXEL_FORMAT_ARGB) ==
               GP_PIXEL_FORMAT_ARGB) {
        DISP_DBGLOG("[GP%zu] Pixel Format is ARGB (0x%X).", s->index,
                    s->pixel_format);
    } else {
        DISP_DBGLOG("[GP%zu] Pixel Format is unknown (0x%X).", s->index,
                    s->pixel_format);
    }
    // TODO: Decompress the data and display it properly.
    uint16_t stride = s->pixel_format & GP_PIXEL_FORMAT_COMPRESSED ?
                          width :
                          s->layers[0].stride;
#if 1
    for (uint16_t y = 0; y < height; y++) {
        uint8_t *dest = memory_region_get_ram_ptr(s->vram);
#if 1
        memcpy(dest + (y * (s->disp_state->width * sizeof(uint32_t))),
               buf + (y * stride), width * sizeof(uint32_t));
#endif
    }
    memory_region_set_dirty(s->vram, 0,
                            s->height * s->width * sizeof(uint32_t));
#endif
    g_free(buf);
    // TODO: bit 10 might be VBlank, and bit 20 that the transfer finished.
    s->disp_state->int_filter |= BIT(10) | BIT(20);
    // TODO: irq 0 might be VBlank, 2 be GP0, 3 be GP1.
    qemu_irq_raise(s->disp_state->irqs[0]);
}

static bool apple_genpipev2_init(GenPipeState *s, size_t index,
                                 MemoryRegion *vram, AddressSpace *dma_as,
                                 AppleDisplayPipeV2State *disp_state)
{
    memset(s, 0, sizeof(*s));
    s->index = index;
    s->vram = vram;
    s->dma_as = dma_as;
    s->bh = qemu_bh_new(apple_gp_draw_bh, s);
    s->disp_state = disp_state;
    return true;
}

static void apple_disp_reg_write(void *opaque, hwaddr addr, uint64_t data,
                                 unsigned size)
{
    AppleDisplayPipeV2State *s;

    s = APPLE_DISPLAYPIPE_V2(opaque);

    if (addr >= 0x200000) {
        addr -= 0x200000;
    }

    switch (addr) {
    case GP_BLOCK_BASE_FOR(0)... GP_BLOCK_END_FOR(0):
        apple_disp_gp_reg_write(&s->genpipes[0], addr, data);
        break;

    case GP_BLOCK_BASE_FOR(1)... GP_BLOCK_END_FOR(1):
        apple_disp_gp_reg_write(&s->genpipes[1], addr, data);
        break;

    case REG_CONTROL_INT_FILTER:
        s->int_filter &= ~(uint32_t)data;
        qemu_irq_lower(s->irqs[0]);
        break;

    default:
        DISP_DBGLOG("[disp] Unknown write @ 0x" HWADDR_FMT_plx
                    " value: 0x" HWADDR_FMT_plx,
                    addr, data);
        break;
    }
}

static uint64_t apple_disp_reg_read(void *opaque, hwaddr addr,
                                    const unsigned size)
{
    AppleDisplayPipeV2State *s;

    s = APPLE_DISPLAYPIPE_V2(opaque);

    if (addr >= 0x200000) {
        addr -= 0x200000;
    }

    switch (addr) {
    case GP_BLOCK_BASE_FOR(0)... GP_BLOCK_END_FOR(0): {
        return apple_disp_gp_reg_read(&s->genpipes[0], addr);
    }
    case GP_BLOCK_BASE_FOR(1)... GP_BLOCK_END_FOR(1): {
        return apple_disp_gp_reg_read(&s->genpipes[1], addr);
    }
    case REG_CONTROL_VERSION: {
        DISP_DBGLOG("[disp] Version -> 0x%x", CONTROL_VERSION_A0);
        return CONTROL_VERSION_A0;
    }
    case REG_CONTROL_FRAME_SIZE: {
        DISP_DBGLOG("[disp] Frame Size -> 0x%x", (s->width << 16) | s->height);
        return (s->width << 16) | s->height;
    }
    case REG_CONTROL_INT_FILTER: {
        DISP_DBGLOG("[disp] Int Filter -> 0x%x", s->int_filter);
        return s->int_filter;
    }
    default:
        DISP_DBGLOG("[disp] Unknown read @ 0x" HWADDR_FMT_plx, addr);
        return 0;
    }
}

static const MemoryRegionOps apple_disp_v2_reg_ops = {
    .write = apple_disp_reg_write,
    .read = apple_disp_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
};

static uint32_t disp_timing_info[] = { 0x33C, 0x90, 0x1, 0x1,
                                       0x700, 0x1,  0x1, 0x1 };

AppleDisplayPipeV2State *apple_displaypipe_v2_create(DTBNode *node)
{
    DeviceState *dev;
    SysBusDevice *sbd;
    AppleDisplayPipeV2State *s;
    DTBProp *prop;
    uint64_t *reg;
    int i;

    dev = qdev_new(TYPE_APPLE_DISPLAYPIPE_V2);
    sbd = SYS_BUS_DEVICE(dev);
    s = APPLE_DISPLAYPIPE_V2(sbd);

    dtb_set_prop(node, "display-target", 15, "DisplayTarget5");
    dtb_set_prop(node, "display-timing-info", sizeof(disp_timing_info),
                 disp_timing_info);
    dtb_set_prop_u32(node, "bics-param-set", 0xD);
    dtb_set_prop_u32(node, "dot-pitch", 326);
    dtb_set_prop_null(node, "function-brightness_update");

    prop = dtb_find_prop(node, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;
    memory_region_init_io(&s->up_regs, OBJECT(sbd), &apple_disp_v2_reg_ops, sbd,
                          "up.regs", reg[1]);
    sysbus_init_mmio(sbd, &s->up_regs);
    object_property_add_const_link(OBJECT(sbd), "up.regs", OBJECT(&s->up_regs));

    s->invalidated = true;

    for (i = 0; i < 9; i++) {
        sysbus_init_irq(sbd, &s->irqs[i]);
    }

    return s;
}

static void apple_displaypipe_v2_draw_row(void *opaque, uint8_t *dest,
                                          const uint8_t *src, int width,
                                          int dest_pitch)
{
    while (width--) {
        uint32_t colour = ldl_le_p(src);
        memcpy(dest, &colour, sizeof(colour));
        src += sizeof(colour);
        dest += sizeof(colour);
    }
}

static void apple_displaypipe_v2_invalidate(void *opaque)
{
    AppleDisplayPipeV2State *s = APPLE_DISPLAYPIPE_V2(opaque);
    s->invalidated = true;
}

static void apple_displaypipe_v2_gfx_update(void *opaque)
{
    AppleDisplayPipeV2State *s = APPLE_DISPLAYPIPE_V2(opaque);
    DisplaySurface *surface = qemu_console_surface(s->console);

    int stride = s->width * sizeof(uint32_t);
    int first = 0, last = 0;

    if (s->invalidated) {
        framebuffer_update_memory_section(&s->vram_section, &s->vram, 0,
                                          s->height, stride);
        s->invalidated = false;
    }

    framebuffer_update_display(surface, &s->vram_section, s->width, s->height,
                               stride, stride, 0, 0,
                               apple_displaypipe_v2_draw_row, s, &first, &last);
    if (first >= 0) {
        dpy_gfx_update(s->console, 0, first, s->width, last - first + 1);
    }
}

static const GraphicHwOps apple_displaypipe_v2_ops = {
    .invalidate = apple_displaypipe_v2_invalidate,
    .gfx_update = apple_displaypipe_v2_gfx_update,
};

static void apple_displaypipe_v2_reset_hold(Object *obj, ResetType type)
{
    AppleDisplayPipeV2State *s = APPLE_DISPLAYPIPE_V2(obj);

    s->invalidated = true;
    s->int_filter = 0;
    qemu_irq_lower(s->irqs[0]);
    apple_genpipev2_init(&s->genpipes[0], 0, &s->vram, &s->dma_as, s);
    apple_genpipev2_init(&s->genpipes[1], 1, &s->vram, &s->dma_as, s);

    memset(memory_region_get_ram_ptr(&s->vram), 0,
           memory_region_size(&s->vram));
    memory_region_set_dirty(&s->vram, 0, memory_region_size(&s->vram));
}

static void apple_displaypipe_v2_realize(DeviceState *dev, Error **errp)
{
    AppleDisplayPipeV2State *s = APPLE_DISPLAYPIPE_V2(dev);

    s->console = graphic_console_init(dev, 0, &apple_displaypipe_v2_ops, s);
    qemu_console_resize(s->console, s->width, s->height);
}

static Property apple_displaypipe_v2_props[] = {
    DEFINE_PROP_UINT32("width", AppleDisplayPipeV2State, width, 828),
    DEFINE_PROP_UINT32("height", AppleDisplayPipeV2State, height, 1792),
    DEFINE_PROP_END_OF_LIST(),
};

static void apple_displaypipe_v2_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    rc->phases.hold = apple_displaypipe_v2_reset_hold;

    device_class_set_props(dc, apple_displaypipe_v2_props);
    dc->realize = apple_displaypipe_v2_realize;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo apple_displaypipe_v2_type_info = {
    .name = TYPE_APPLE_DISPLAYPIPE_V2,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleDisplayPipeV2State),
    .class_init = apple_displaypipe_v2_class_init,
};

static void apple_displaypipe_v2_register_types(void)
{
    type_register_static(&apple_displaypipe_v2_type_info);
}

type_init(apple_displaypipe_v2_register_types);
