/*
 * Apple Multi Touch SPI Controller.
 *
 * Copyright (c) 2024 Visual Ehrmanntraut (VisualEhrmanntraut).
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

#include "hw/arm/apple-silicon/mt-spi.h"
#include "hw/irq.h"
#include "hw/ssi/ssi.h"
#include "qemu/cutils.h"
#include "qemu/lockable.h"

static const uint16_t crc16_table[256] = {
    0x0000, 0xC0C1, 0xC181, 0x0140, 0xC301, 0x03C0, 0x0280, 0xC241, 0xC601,
    0x06C0, 0x0780, 0xC741, 0x0500, 0xC5C1, 0xC481, 0x0440, 0xCC01, 0x0CC0,
    0x0D80, 0xCD41, 0x0F00, 0xCFC1, 0xCE81, 0x0E40, 0x0A00, 0xCAC1, 0xCB81,
    0x0B40, 0xC901, 0x09C0, 0x0880, 0xC841, 0xD801, 0x18C0, 0x1980, 0xD941,
    0x1B00, 0xDBC1, 0xDA81, 0x1A40, 0x1E00, 0xDEC1, 0xDF81, 0x1F40, 0xDD01,
    0x1DC0, 0x1C80, 0xDC41, 0x1400, 0xD4C1, 0xD581, 0x1540, 0xD701, 0x17C0,
    0x1680, 0xD641, 0xD201, 0x12C0, 0x1380, 0xD341, 0x1100, 0xD1C1, 0xD081,
    0x1040, 0xF001, 0x30C0, 0x3180, 0xF141, 0x3300, 0xF3C1, 0xF281, 0x3240,
    0x3600, 0xF6C1, 0xF781, 0x3740, 0xF501, 0x35C0, 0x3480, 0xF441, 0x3C00,
    0xFCC1, 0xFD81, 0x3D40, 0xFF01, 0x3FC0, 0x3E80, 0xFE41, 0xFA01, 0x3AC0,
    0x3B80, 0xFB41, 0x3900, 0xF9C1, 0xF881, 0x3840, 0x2800, 0xE8C1, 0xE981,
    0x2940, 0xEB01, 0x2BC0, 0x2A80, 0xEA41, 0xEE01, 0x2EC0, 0x2F80, 0xEF41,
    0x2D00, 0xEDC1, 0xEC81, 0x2C40, 0xE401, 0x24C0, 0x2580, 0xE541, 0x2700,
    0xE7C1, 0xE681, 0x2640, 0x2200, 0xE2C1, 0xE381, 0x2340, 0xE101, 0x21C0,
    0x2080, 0xE041, 0xA001, 0x60C0, 0x6180, 0xA141, 0x6300, 0xA3C1, 0xA281,
    0x6240, 0x6600, 0xA6C1, 0xA781, 0x6740, 0xA501, 0x65C0, 0x6480, 0xA441,
    0x6C00, 0xACC1, 0xAD81, 0x6D40, 0xAF01, 0x6FC0, 0x6E80, 0xAE41, 0xAA01,
    0x6AC0, 0x6B80, 0xAB41, 0x6900, 0xA9C1, 0xA881, 0x6840, 0x7800, 0xB8C1,
    0xB981, 0x7940, 0xBB01, 0x7BC0, 0x7A80, 0xBA41, 0xBE01, 0x7EC0, 0x7F80,
    0xBF41, 0x7D00, 0xBDC1, 0xBC81, 0x7C40, 0xB401, 0x74C0, 0x7580, 0xB541,
    0x7700, 0xB7C1, 0xB681, 0x7640, 0x7200, 0xB2C1, 0xB381, 0x7340, 0xB101,
    0x71C0, 0x7080, 0xB041, 0x5000, 0x90C1, 0x9181, 0x5140, 0x9301, 0x53C0,
    0x5280, 0x9241, 0x9601, 0x56C0, 0x5780, 0x9741, 0x5500, 0x95C1, 0x9481,
    0x5440, 0x9C01, 0x5CC0, 0x5D80, 0x9D41, 0x5F00, 0x9FC1, 0x9E81, 0x5E40,
    0x5A00, 0x9AC1, 0x9B81, 0x5B40, 0x9901, 0x59C0, 0x5880, 0x9841, 0x8801,
    0x48C0, 0x4980, 0x8941, 0x4B00, 0x8BC1, 0x8A81, 0x4A40, 0x4E00, 0x8EC1,
    0x8F81, 0x4F40, 0x8D01, 0x4DC0, 0x4C80, 0x8C41, 0x4400, 0x84C1, 0x8581,
    0x4540, 0x8701, 0x47C0, 0x4680, 0x8641, 0x8201, 0x42C0, 0x4380, 0x8341,
    0x4100, 0x81C1, 0x8081, 0x4040,
};

static uint16_t crc16_byte(uint16_t crc, const uint8_t data)
{
    return (crc >> 8) ^ crc16_table[(crc ^ data) & 0xff];
}

static uint16_t crc16(uint16_t crc, uint8_t const *buffer, size_t len)
{
    while (len--) {
        crc = crc16_byte(crc, *buffer++);
    }
    return crc;
}

typedef struct AppleMTSPIBuffer {
    uint8_t *data;
    size_t capacity;
    size_t len;
    size_t read_pos;
} AppleMTSPIBuffer;

struct AppleMTSPIState {
    SSIPeripheral parent_obj;

    QemuMutex lock;
    /// IRQ of the Multi Touch Controller is Active Low.
    /// qemu_irq_raise means IRQ inactive,
    /// qemu_irq_lower means IRQ active.
    qemu_irq irq;
    AppleMTSPIBuffer tx;
    AppleMTSPIBuffer rx;
    AppleMTSPIBuffer pending_hbpp;
    AppleMTSPIBuffer pending_fw;
};

#define DBG_FUNC_ENTRY() fprintf(stderr, "%s: entered\n", __func__)
#define DBG_FUNC_EXIT() fprintf(stderr, "%s@%d: exited\n", __func__, __LINE__)
#define DBG_UINT(expr) fprintf(stderr, "%s: (" #expr ") = %d\n", __func__, expr)
#define DBG_USIZE(expr) \
    fprintf(stderr, "%s: (" #expr ") = %ld\n", __func__, expr)
#define DBG_USIZE_HEX(expr) \
    fprintf(stderr, "%s: (" #expr ") = %lX\n", __func__, expr)
#define DBG_UINT_HEX(expr) \
    fprintf(stderr, "%s: (" #expr ") = 0x%X\n", __func__, expr)
#define DBG_PTR(expr) fprintf(stderr, "%s: (" #expr ") = %p\n", __func__, expr)

// HBPP Command:
// u8 packet_type
// u1 unk0
// u3 packet_size? // 0 = Empty
// u4 unk1

#define HBPP_PACKET_RESET (0x00)
#define HBPP_PACKET_NOP (0x18)
#define HBPP_PACKET_INT_ACK (0x1A)
#define HBPP_PACKET_MEM_READ (0x1C)
#define HBPP_PACKET_MEM_RMW (0x1E)
#define HBPP_PACKET_REQ_CAL (0x1F)
#define HBPP_PACKET_DATA (0x30)

#define HBPP_PACKET_REQ_BOOT (0x011F)
#define HBPP_PACKET_ACK_RD_REQ (0x394C)
#define HBPP_PACKET_ACK_NOP (0x7948)
#define HBPP_PACKET_CAL_DONE (0x6949)
#define HBPP_PACKET_ACK_DATA (0xC14B)
#define HBPP_PACKET_ACK_WR_REQ (0xD14A)

#define LL_PACKET_PREAMBLE (0xDEADBEEF)
#define LL_PACKET_LEN (0x204)

#define LL_PACKET_LOSSLESS_OUTPUT (0x10)
#define LL_PACKET_LOSSY_OUTPUT (0x11)
#define LL_PACKET_LOSSLESS_INPUT (0x20)
#define LL_PACKET_LOSSY_INPUT (0x21)
#define LL_PACKET_CONTROL (0x40)
#define LL_PACKET_NO_DATA (0x80)

#define LL_PACKET_ERROR (0xB3655245)
#define LL_PACKET_ACK (0xD56827AC)
#define LL_PACKET_NAK (0xE4FB139)
#define LL_PACKET_BUSY (0xF8E5179C)

#define HID_PACKET_GET_RESULT_DATA (0x10)
#define HID_PACKET_SET_RESULT_DATA (0x20)
#define HID_PACKET_GET_INPUT_REPORT (0x30)
#define HID_PACKET_GET_OUTPUT_REPORT (0x31)
#define HID_PACKET_GET_FEATURE_REPORT (0x32)
#define HID_PACKET_SET_INPUT_REPORT (0x50)
#define HID_PACKET_SET_OUTPUT_REPORT (0x51)
#define HID_PACKET_SET_FEATURE_REPORT (0x52)

#define HID_PACKET_STATUS_SUCCESS (0)
#define HID_PACKET_STATUS_BUSY (1)
#define HID_PACKET_STATUS_ERROR (2)
#define HID_PACKET_STATUS_ERROR_ID_MISMATCH (3)
#define HID_PACKET_STATUS_ERROR_UNSUPPORTED (4)
#define HID_PACKET_STATUS_ERROR_INCORRECT_LENGTH (5)

static void apple_mt_spi_buf_free(AppleMTSPIBuffer *buf)
{
    g_free(buf->data);
    memset(buf, 0, sizeof(*buf));
}

static void apple_mt_spi_buf_ensure_capacity(AppleMTSPIBuffer *buf,
                                             size_t bytes)
{
    if ((buf->len + bytes) > buf->capacity) {
        buf->capacity = buf->len + bytes;
        buf->data = g_realloc(buf->data, buf->capacity);
    }
}

static void apple_mt_spi_buf_set_capacity(AppleMTSPIBuffer *buf,
                                          size_t capacity)
{
    g_assert_cmphex(capacity, >=, buf->capacity);
    buf->capacity = capacity;
    buf->data = g_realloc(buf->data, buf->capacity);
}

static void apple_mt_spi_buf_set_len(AppleMTSPIBuffer *buf, uint8_t val,
                                     size_t len)
{
    g_assert_cmphex(len, >=, buf->len);
    apple_mt_spi_buf_ensure_capacity(buf, len - buf->len);
    memset(buf->data + buf->len, val, len - buf->len);
    buf->len = len;
}

static size_t apple_mt_spi_buf_get_pos(const AppleMTSPIBuffer *buf)
{
    g_assert_cmphex(buf->len, !=, 0);
    return buf->len - 1;
}

static bool apple_mt_spi_buf_is_empty(const AppleMTSPIBuffer *buf)
{
    return buf->len == 0;
}

static bool apple_mt_spi_buf_is_full(const AppleMTSPIBuffer *buf)
{
    return buf->len == buf->capacity;
}

static bool apple_mt_spi_buf_pos_at_start(const AppleMTSPIBuffer *buf)
{
    return apple_mt_spi_buf_get_pos(buf) == 0;
}

static bool apple_mt_spi_buf_read_pos_at_end(const AppleMTSPIBuffer *buf)
{
    return (buf->read_pos + 1) == buf->len;
}

static void apple_mt_spi_buf_push_byte(AppleMTSPIBuffer *buf, uint8_t val)
{
    apple_mt_spi_buf_ensure_capacity(buf, sizeof(val));
    buf->data[buf->len] = val;
    buf->len += sizeof(val);
}

static void apple_mt_spi_buf_push_word(AppleMTSPIBuffer *buf, uint16_t val)
{
    apple_mt_spi_buf_ensure_capacity(buf, sizeof(val));
    stw_le_p(buf->data + buf->len, val);
    buf->len += sizeof(val);
}

static void apple_mt_spi_buf_push_dword(AppleMTSPIBuffer *buf, uint32_t val)
{
    apple_mt_spi_buf_ensure_capacity(buf, sizeof(val));
    stl_le_p(buf->data + buf->len, val);
    buf->len += sizeof(val);
}

static void apple_mt_spi_buf_push_crc16(AppleMTSPIBuffer *buf)
{
    g_assert_false(apple_mt_spi_buf_is_empty(buf));
    apple_mt_spi_buf_push_word(buf, crc16(0, buf->data, buf->len));
}

static void apple_mt_spi_buf_append(AppleMTSPIBuffer *buf,
                                    AppleMTSPIBuffer *other_buf)
{
    if (!apple_mt_spi_buf_is_empty(other_buf)) {
        apple_mt_spi_buf_ensure_capacity(buf, other_buf->len);
        memcpy(buf->data + buf->len, other_buf->data, other_buf->len);
        buf->len += other_buf->len;
    }
    apple_mt_spi_buf_free(other_buf);
}

static uint8_t apple_mt_spi_buf_pop(AppleMTSPIBuffer *buf)
{
    uint8_t ret;

    if (apple_mt_spi_buf_is_empty(buf)) {
        return 0;
    }

    g_assert_nonnull(buf->data);
    g_assert_cmphex(buf->len, >, buf->read_pos);

    ret = buf->data[buf->read_pos];

    if (apple_mt_spi_buf_read_pos_at_end(buf) ||
        apple_mt_spi_buf_is_empty(buf)) {
        apple_mt_spi_buf_free(buf);
    } else {
        buf->read_pos += 1;
    }

    return ret;
}

static inline uint8_t apple_mt_spi_buf_read_byte(const AppleMTSPIBuffer *buf,
                                                 size_t off)
{
    g_assert_nonnull(buf->data);
    g_assert_cmphex(off, <, buf->len);
    return buf->data[off];
}

static inline uint16_t apple_mt_spi_buf_read_word(const AppleMTSPIBuffer *buf,
                                                  size_t off)
{
    g_assert_nonnull(buf->data);
    g_assert_cmphex(off + sizeof(uint16_t), <, buf->len);
    return lduw_be_p(buf->data + off);
}

static inline uint32_t apple_mt_spi_buf_read_dword(const AppleMTSPIBuffer *buf,
                                                   size_t off)
{
    g_assert_nonnull(buf->data);
    g_assert_cmphex(off + sizeof(uint32_t), <, buf->len);
    return apple_mt_spi_buf_read_word(buf, off) |
           (apple_mt_spi_buf_read_word(buf, off + sizeof(uint16_t)) << 16);
}

static void apple_mt_spi_enter_reset(Object *obj, ResetType type)
{
    AppleMTSPIState *s;

    s = APPLE_MT_SPI(obj);

    qemu_irq_raise(s->irq);

    apple_mt_spi_buf_free(&s->tx);
    apple_mt_spi_buf_free(&s->rx);
    apple_mt_spi_buf_free(&s->pending_hbpp);
}

static void apple_mt_spi_push_pending_hbpp_word(AppleMTSPIState *s,
                                                uint16_t val)
{
    apple_mt_spi_buf_push_word(&s->pending_hbpp, val);
    qemu_irq_lower(s->irq);
}

static void apple_mt_spi_push_pending_hbpp_dword(AppleMTSPIState *s,
                                                 uint32_t val)
{
    apple_mt_spi_buf_push_dword(&s->pending_hbpp, val);
    qemu_irq_lower(s->irq);
}

static inline uint16_t apple_mt_spi_hbpp_packet_hdr_len(uint8_t val)
{
    switch (val) {
    case HBPP_PACKET_RESET:
        return 0x4;
    case HBPP_PACKET_NOP:
        return 0x2;
    case HBPP_PACKET_INT_ACK:
        return 0x2;
    case HBPP_PACKET_MEM_READ:
        return 0x8;
    case HBPP_PACKET_MEM_RMW:
        return 0x10;
    case HBPP_PACKET_REQ_CAL:
        return 0x2;
    case HBPP_PACKET_DATA:
        return 0xA;
    default:
        DBG_UINT(val);
        g_assert_not_reached();
    }
}

static void apple_mt_spi_handle_hbpp_data(AppleMTSPIState *s)
{
    uint16_t payload_len;
    uint32_t address;
    uint16_t new_rx_capacity;

    if (!apple_mt_spi_buf_is_full(&s->rx)) {
        return;
    }

    payload_len = apple_mt_spi_buf_read_word(&s->rx, 2) * sizeof(uint32_t);
    address = apple_mt_spi_buf_read_dword(&s->rx, 4);

    fprintf(stderr, "%s: 0x%08X <- 0x%04X bytes\n", __func__, address,
            payload_len);

    new_rx_capacity = apple_mt_spi_hbpp_packet_hdr_len(HBPP_PACKET_DATA) +
                      payload_len + sizeof(uint32_t);

    if (s->rx.capacity == new_rx_capacity) {
        apple_mt_spi_push_pending_hbpp_word(s, HBPP_PACKET_ACK_DATA);
    } else {
        apple_mt_spi_buf_set_capacity(&s->rx, new_rx_capacity);
    }
}

static void apple_mt_spi_handle_hbpp_mem_rmw(AppleMTSPIState *s)
{
    uint32_t address;
    uint32_t mask;
    uint32_t value;

    if (!apple_mt_spi_buf_is_full(&s->rx)) {
        return;
    }

    address = apple_mt_spi_buf_read_dword(&s->rx, 2);
    mask = apple_mt_spi_buf_read_dword(&s->rx, 6);
    value = apple_mt_spi_buf_read_dword(&s->rx, 10);

    fprintf(stderr, "%s: 0x%X[0x%X] <- 0x%X\n", __func__, address, mask, value);
    apple_mt_spi_push_pending_hbpp_word(s, HBPP_PACKET_ACK_WR_REQ);
}

static void apple_mt_spi_handle_hbpp(AppleMTSPIState *s)
{
    uint8_t packet_type;

    packet_type = apple_mt_spi_buf_read_byte(&s->rx, 0);

    if (apple_mt_spi_buf_pos_at_start(&s->rx)) {
        apple_mt_spi_buf_set_capacity(
            &s->rx, apple_mt_spi_hbpp_packet_hdr_len(packet_type));
    }

    switch (packet_type) {
    case HBPP_PACKET_RESET:
        if (apple_mt_spi_buf_is_full(&s->rx)) {
            fprintf(stderr, "%s: Reset\n", __func__);
            apple_mt_spi_enter_reset(OBJECT(s), RESET_TYPE_COLD);
            apple_mt_spi_push_pending_hbpp_word(s, HBPP_PACKET_REQ_BOOT);
        }
        break;
    case HBPP_PACKET_NOP:
        if (apple_mt_spi_buf_pos_at_start(&s->rx)) {
            fprintf(stderr, "%s: NOP\n", __func__);
            if (apple_mt_spi_buf_is_empty(&s->tx)) {
                apple_mt_spi_buf_push_word(&s->tx, HBPP_PACKET_ACK_NOP);
            }
        }
        break;
    case HBPP_PACKET_INT_ACK:
        if (apple_mt_spi_buf_pos_at_start(&s->rx)) {
            fprintf(stderr, "%s: Int Ack\n", __func__);
            apple_mt_spi_buf_append(&s->tx, &s->pending_hbpp);
        }
        break;
    case HBPP_PACKET_MEM_READ:
        if (apple_mt_spi_buf_pos_at_start(&s->rx)) {
            fprintf(stderr, "%s: Mem Read\n", __func__);
            apple_mt_spi_push_pending_hbpp_word(s, HBPP_PACKET_ACK_RD_REQ);
            apple_mt_spi_push_pending_hbpp_dword(s, 0x00000000); // value
            apple_mt_spi_push_pending_hbpp_word(s, 0x0000); // crc16 of value
        }
        break;
    case HBPP_PACKET_MEM_RMW:
        apple_mt_spi_handle_hbpp_mem_rmw(s);
        break;
    case HBPP_PACKET_REQ_CAL:
        if (apple_mt_spi_buf_pos_at_start(&s->rx)) {
            fprintf(stderr, "%s: Request Calibration\n", __func__);
            apple_mt_spi_push_pending_hbpp_word(s, HBPP_PACKET_CAL_DONE);
        }
        break;
    case HBPP_PACKET_DATA:
        apple_mt_spi_handle_hbpp_data(s);
        break;
    default:
        fprintf(stderr, "%s: Unknown packet type 0x%02X\n", __func__,
                packet_type);
        break;
    }
}

static void apple_mt_spi_push_preamble(AppleMTSPIBuffer *buf)
{
    apple_mt_spi_buf_push_dword(buf, LL_PACKET_PREAMBLE);
}

static void apple_mt_spi_push_ll_hdr(AppleMTSPIBuffer *buf, uint8_t type,
                                     uint8_t interface, uint16_t payload_off,
                                     uint16_t payload_remaining,
                                     uint16_t payload_length)
{
    apple_mt_spi_buf_push_byte(buf, type);
    apple_mt_spi_buf_push_byte(buf, interface);
    apple_mt_spi_buf_push_word(buf, payload_off);
    apple_mt_spi_buf_push_word(buf, payload_remaining);
    apple_mt_spi_buf_push_word(buf, payload_length);
}

static void apple_mt_spi_pad_ll_packet(AppleMTSPIBuffer *buf)
{
    apple_mt_spi_buf_set_len(
        buf, 0, LL_PACKET_LEN - sizeof(uint32_t) - sizeof(uint16_t));
}

static void apple_mt_spi_push_no_data(AppleMTSPIBuffer *buf)
{
    apple_mt_spi_push_ll_hdr(buf, LL_PACKET_NO_DATA, 0, 0, 0, 0);
    apple_mt_spi_pad_ll_packet(buf);
    apple_mt_spi_buf_push_crc16(buf);
}

static uint8_t apple_mt_spi_ll_read_payload_byte(AppleMTSPIBuffer *buf,
                                                 size_t off)
{
    size_t offset;

    g_assert_false(apple_mt_spi_buf_is_empty(buf));
    g_assert_cmphex(6, <=, buf->len);
    offset = sizeof(uint32_t) + sizeof(uint64_t) + lduw_be_p(buf->data + 6);
    g_assert_cmphex(offset + off, <, buf->len);
    return buf->data[offset + off];
}

static uint16_t apple_mt_spi_ll_read_payload_word(AppleMTSPIBuffer *buf,
                                                  size_t off)
{
    size_t offset;

    g_assert_false(apple_mt_spi_buf_is_empty(buf));
    g_assert_cmphex(6, <=, buf->len);
    offset = sizeof(uint32_t) + sizeof(uint64_t) + lduw_be_p(buf->data + 6);
    g_assert_cmphex(offset + off + sizeof(uint16_t), <=, buf->len);
    return lduw_le_p(buf->data + offset + off);
}

static void apple_mt_spi_push_hid_hdr(AppleMTSPIBuffer *buf, uint8_t type,
                                      uint8_t report_id, uint8_t packet_status,
                                      uint8_t frame_number,
                                      uint16_t length_requested,
                                      uint16_t payload_length)
{
    apple_mt_spi_buf_push_byte(buf, type);
    apple_mt_spi_buf_push_byte(buf, report_id);
    apple_mt_spi_buf_push_byte(buf, packet_status);
    apple_mt_spi_buf_push_byte(buf, frame_number);
    apple_mt_spi_buf_push_word(buf, length_requested);
    apple_mt_spi_buf_push_word(buf, payload_length);
}

static void apple_mt_spi_handle_fw_packet(AppleMTSPIState *s)
{
    uint8_t packet_type;
    AppleMTSPIBuffer ll_buf;

    if (apple_mt_spi_buf_get_pos(&s->rx) == sizeof(uint32_t)) {
        apple_mt_spi_buf_set_capacity(&s->rx, LL_PACKET_LEN);

        memset(&ll_buf, 0, sizeof(ll_buf));

        if (apple_mt_spi_buf_is_empty(&s->pending_fw)) {
            apple_mt_spi_push_no_data(&ll_buf);
        } else {
            apple_mt_spi_push_ll_hdr(&ll_buf, LL_PACKET_LOSSLESS_OUTPUT, 0, 0,
                                     0, s->pending_fw.len);
            // qemu_hexdump(stderr, "PendingFW", s->pending_fw.data,
            //              s->pending_fw.len);
            apple_mt_spi_buf_append(&ll_buf, &s->pending_fw);
            apple_mt_spi_pad_ll_packet(&ll_buf);
            apple_mt_spi_buf_push_crc16(&ll_buf);
        }

        // qemu_hexdump(stderr, "LL TX", ll_buf.data, ll_buf.len);
        apple_mt_spi_buf_append(&s->tx, &ll_buf);
    }

    packet_type = apple_mt_spi_buf_read_byte(&s->rx, sizeof(uint32_t));

    switch (packet_type) {
    case LL_PACKET_NO_DATA:
        if (apple_mt_spi_buf_get_pos(&s->rx) == sizeof(uint32_t)) {
            fprintf(stderr, "%s: No Data\n", __func__);
        }
        break;
    case LL_PACKET_CONTROL:
        if (apple_mt_spi_buf_is_full(&s->rx)) {
            fprintf(stderr, "%s: Control\n", __func__);

            memset(&ll_buf, 0, sizeof(ll_buf));

            uint8_t type = apple_mt_spi_ll_read_payload_byte(&s->rx, 0);
            uint8_t report_id =
                apple_mt_spi_ll_read_payload_byte(&s->rx, sizeof(uint8_t));
            uint8_t frame_number =
                apple_mt_spi_ll_read_payload_byte(&s->rx, sizeof(uint8_t) * 3);
            // uint16_t requested_len =
            //     apple_mt_spi_ll_read_payload_word(&s->rx, sizeof(uint8_t) * 4);

            DBG_UINT_HEX(type);
            DBG_UINT_HEX(report_id);
            DBG_UINT_HEX(frame_number);
            // DBG_UINT_HEX(requested_len);

            apple_mt_spi_push_hid_hdr(&ll_buf, HID_PACKET_SET_RESULT_DATA,
                                      report_id, HID_PACKET_STATUS_SUCCESS,
                                      frame_number, 0, 8);
            apple_mt_spi_buf_push_dword(&ll_buf, 0xDEADBEEF);
            apple_mt_spi_buf_push_dword(&ll_buf, 0xFEEDFACE);
            apple_mt_spi_buf_push_crc16(&ll_buf);
            apple_mt_spi_buf_append(&s->pending_fw, &ll_buf);
        }
        break;
    default:
        if (apple_mt_spi_buf_get_pos(&s->rx) == sizeof(uint32_t)) {
            fprintf(stderr, "%s: Unknown type 0x%X\n", __func__, packet_type);
        }
        break;
    }
}

static void apple_mt_spi_handle_fw(AppleMTSPIState *s)
{
    uint8_t packet_type;

    if (apple_mt_spi_buf_pos_at_start(&s->rx)) {
        apple_mt_spi_buf_set_capacity(&s->rx, sizeof(uint32_t) * 2);
        apple_mt_spi_push_preamble(&s->tx);
    }

    if (apple_mt_spi_buf_get_pos(&s->rx) < sizeof(uint32_t)) {
        return;
    }

    packet_type = apple_mt_spi_buf_read_byte(&s->rx, sizeof(uint32_t));

    switch (packet_type) {
    case LL_PACKET_ERROR & 0xFF:
    case LL_PACKET_ACK & 0xFF:
    case LL_PACKET_NAK & 0xFF:
    case LL_PACKET_BUSY & 0xFF:
        if (apple_mt_spi_buf_get_pos(&s->rx) == sizeof(uint32_t)) {
            apple_mt_spi_buf_push_dword(&s->tx, LL_PACKET_ACK);
        }
        break;
    default:
        apple_mt_spi_handle_fw_packet(s);
        break;
    }

    // if (apple_mt_spi_buf_is_full(&s->rx)) {
    //     qemu_hexdump(stderr, "RX", s->rx.data, s->rx.len);
    // }
}

static uint32_t apple_mt_spi_transfer(SSIPeripheral *dev, uint32_t val)
{
    AppleMTSPIState *s;
    uint8_t ret;

    s = APPLE_MT_SPI(dev);

    QEMU_LOCK_GUARD(&s->lock);

    apple_mt_spi_buf_push_byte(&s->rx, (uint8_t)val);

    if (apple_mt_spi_buf_read_byte(&s->rx, 0) == (LL_PACKET_PREAMBLE & 0xFF)) {
        apple_mt_spi_handle_fw(s);
    } else {
        apple_mt_spi_handle_hbpp(s);
    }

    if (apple_mt_spi_buf_is_full(&s->rx)) {
        apple_mt_spi_buf_free(&s->rx);
    }

    ret = apple_mt_spi_buf_pop(&s->tx);

    if (apple_mt_spi_buf_is_empty(&s->pending_hbpp) &&
        apple_mt_spi_buf_is_empty(&s->pending_fw)) {
        qemu_irq_raise(s->irq);
    } else {
        qemu_irq_lower(s->irq);
    }

    return ret;
}

static void apple_mt_spi_realize(SSIPeripheral *dev, Error **errp)
{
}

static void apple_mt_spi_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    rc->phases.enter = apple_mt_spi_enter_reset;

    k->realize = apple_mt_spi_realize;
    k->transfer = apple_mt_spi_transfer;

    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static void apple_mt_instance_init(Object *obj)
{
    AppleMTSPIState *s;

    s = APPLE_MT_SPI(obj);

    qdev_init_gpio_out_named(DEVICE(s), &s->irq, APPLE_MT_SPI_IRQ, 1);

    qemu_mutex_init(&s->lock);
}

static const TypeInfo apple_mt_spi_type_info = {
    .name = TYPE_APPLE_MT_SPI,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(AppleMTSPIState),
    .instance_init = apple_mt_instance_init,
    .class_init = apple_mt_spi_class_init,
};

static void apple_mt_spi_register_types(void)
{
    type_register_static(&apple_mt_spi_type_info);
}

type_init(apple_mt_spi_register_types);
