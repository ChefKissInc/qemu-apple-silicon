#include "qemu/osdep.h"
#include "hw/usb/apple-otg.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/lockable.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/arm/xnu_dtb.h"

#define rAUSB_USB20PHY_CTL		(0x00)
#define rAUSB_USB20PHY_OTGSIG	(0x04)
#define rAUSB_USB20PHY_CFG0		(0x08)
#define rAUSB_USB20PHY_CFG1		(0x0C)
#define rAUSB_USB20PHY_BATCTL	(0x10)
#define rAUSB_USB20PHY_TEST		(0x1C)

static void apple_otg_realize(DeviceState *dev, Error **errp)
{

}
static void apple_otg_reset(DeviceState *dev)
{

}
static void phy_reg_write(void *opaque,
                  hwaddr addr,
                  uint64_t data,
                  unsigned size){
    fprintf(stderr, "OTG: phy reg WRITE @ 0x" TARGET_FMT_plx " value: 0x" TARGET_FMT_plx "\n", addr, data);    
    AppleOTGState *s = APPLE_OTG(opaque);
    memcpy(s->phy_reg + addr, &data, size);
}
static uint64_t phy_reg_read(void *opaque,
                     hwaddr addr,
                     unsigned size){
    fprintf(stderr, "OTG: phy reg READ @ 0x" TARGET_FMT_plx "\n", addr);
    AppleOTGState *s = APPLE_OTG(opaque);
    uint64_t val = 0;
    memcpy(&val, s->phy_reg + addr, size);
    return val;
}
static const MemoryRegionOps phy_reg_ops = {
    .write = phy_reg_write,
    .read = phy_reg_read,
};

static void usbctl_reg_write(void *opaque,
                  hwaddr addr,
                  uint64_t data,
                  unsigned size){
    fprintf(stderr, "OTG: usbctl reg WRITE @ 0x" TARGET_FMT_plx " value: 0x" TARGET_FMT_plx "\n", addr, data);    
    AppleOTGState *s = APPLE_OTG(opaque);
    memcpy(s->usbctl_reg + addr, &data, size);
}
static uint64_t usbctl_reg_read(void *opaque,
                     hwaddr addr,
                     unsigned size){
    fprintf(stderr, "OTG: usbctl reg READ @ 0x" TARGET_FMT_plx "\n", addr);
    AppleOTGState *s = APPLE_OTG(opaque);
    uint64_t val = 0;
    memcpy(&val, s->usbctl_reg + addr, size);
    return val;
}
static const MemoryRegionOps usbctl_reg_ops = {
    .write = usbctl_reg_write,
    .read = usbctl_reg_read,
};

DeviceState *apple_otg_create(void){
    DeviceState *dev;
    SysBusDevice *sbd;
    AppleOTGState *s;

    dev = qdev_new(TYPE_APPLE_OTG);
    sbd = SYS_BUS_DEVICE(dev);
    s = APPLE_OTG(dev);

    s->phy = g_new(MemoryRegion, 1);
    memory_region_init_io(s->phy, OBJECT(dev), &phy_reg_ops, s, TYPE_APPLE_OTG ".phy", sizeof(s->phy_reg));
    sysbus_init_mmio(sbd, s->phy);
    *(uint32_t*)(s->phy_reg + rAUSB_USB20PHY_OTGSIG) |= (1 << 8); //cable connected
    s->usbctl = g_new(MemoryRegion, 1);
    memory_region_init_io(s->usbctl, OBJECT(dev), &usbctl_reg_ops, s, TYPE_APPLE_OTG ".usbctl", sizeof(s->usbctl_reg));
    sysbus_init_mmio(sbd, s->usbctl);
    return dev;
}

static void apple_otg_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = apple_otg_realize;
    dc->reset = apple_otg_reset;
    dc->desc = "Apple Synopsys USB OTG";
}

static const TypeInfo apple_otg_info = {
    .name = TYPE_APPLE_OTG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleOTGState),
    .class_init = apple_otg_class_init,
};

static void apple_otg_register_types(void)
{
    type_register_static(&apple_otg_info);
}

type_init(apple_otg_register_types);