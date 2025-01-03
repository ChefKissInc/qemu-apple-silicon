/*
 * Copyright (c) 2019 Jonathan Afek <jonyafek@me.com>
 * Copyright (c) 2024 Visual Ehrmanntraut (VisualEhrmanntraut).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef HW_ARM_APPLE_SILICON_DTB_H
#define HW_ARM_APPLE_SILICON_DTB_H

#include "qemu/osdep.h"
#include "exec/hwaddr.h"

#define DT_PROP_FLAG_PLACEHOLDER (1 << 31)
#define DT_PROP_FLAGS_MASK (0xF0000000)
#define DT_PROP_SIZE_MASK (~DT_PROP_FLAGS_MASK)

#define DTB_PROP_NAME_LEN (32)

typedef struct {
    uint8_t name[DTB_PROP_NAME_LEN];
    uint32_t length;
    uint32_t flags;
    uint8_t *value;
} DTBProp;

typedef struct {
    uint32_t prop_count;
    uint32_t child_node_count;
    GList *props;
    GList *child_nodes;
} DTBNode;

DTBNode *dtb_unserialise(uint8_t *dtb_blob);
void dtb_serialise(uint8_t *buf, DTBNode *root);
bool dtb_remove_node_named(DTBNode *parent, const char *name);
void dtb_remove_node(DTBNode *node, DTBNode *child);
void dtb_unset_prop(DTBNode *node, DTBProp *prop);
DTBProp *dtb_set_prop(DTBNode *n, const char *name, uint32_t size,
                      const void *val);
DTBProp *dtb_set_prop_null(DTBNode *node, const char *name);
DTBProp *dtb_set_prop_u32(DTBNode *node, const char *name, const uint32_t val);
DTBProp *dtb_set_prop_u64(DTBNode *node, const char *name, const uint64_t val);
DTBProp *dtb_set_prop_hwaddr(DTBNode *node, const char *name, const hwaddr val);
DTBNode *dtb_find_node(DTBNode *n, const char *path);
DTBNode *dtb_get_node(DTBNode *n, const char *path);
uint64_t dtb_get_serialised_node_size(DTBNode *node);
DTBProp *dtb_find_prop(DTBNode *node, const char *name);

#endif /* HW_ARM_APPLE_SILICON_DTB_H */
