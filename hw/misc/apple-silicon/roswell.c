/*
 * Apple Roswell.
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

#include "qemu/osdep.h"
#include "hw/misc/apple-silicon/roswell.h"

static uint8_t apple_roswell_rx(I2CSlave *i2c)
{
    return 0x00;
}

static void apple_roswell_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *c = I2C_SLAVE_CLASS(klass);

    c->recv = apple_roswell_rx;
    dc->desc = "Apple Roswell";
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo apple_roswell_type_info = {
    .name = TYPE_APPLE_ROSWELL,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(AppleRoswellState),
    .class_init = apple_roswell_class_init,
};

static void apple_roswell_register_types(void)
{
    type_register_static(&apple_roswell_type_info);
}

type_init(apple_roswell_register_types);
