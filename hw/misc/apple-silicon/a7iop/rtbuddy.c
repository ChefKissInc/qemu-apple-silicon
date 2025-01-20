#include "hw/misc/apple-silicon/a7iop/core.h"
#include "hw/misc/apple-silicon/a7iop/mailbox/core.h"
#include "hw/misc/apple-silicon/a7iop/private.h"
#include "hw/misc/apple-silicon/a7iop/rtbuddy.h"
#include "hw/resettable.h"
#include "qemu/lockable.h"
#include "qemu/main-loop.h"
#include "trace.h"

#define MSG_SEND_HELLO 1
#define MSG_RECV_HELLO 2

#define MSG_TYPE_PING 3
#define MSG_TYPE_PING_ACK 4

#define MSG_TYPE_EPSTART 5

#define MSG_TYPE_SET_IOP_PSTATE 6

#define MSG_GET_PSTATE(_x) ((_x) & 0xFFF)
#define PSTATE_SLPNOMEM 0x0
// #define PSTATE_ON 0x20
#define PSTATE_WAIT_VR 0x201
#define PSTATE_PWRGATE 0x202 // Eh?
#define PSTATE_ON 0x220

#define MSG_TYPE_SET_AP_PSTATE_ACK 7
#define MSG_TYPE_ROLLCALL 8
#define MSG_TYPE_SET_AP_PSTATE 11

static inline AppleA7IOPMessage *apple_rtbuddy_construct_msg(uint32_t ep,
                                                             uint64_t data)
{
    AppleA7IOPMessage *msg;

    msg = g_new0(AppleA7IOPMessage, 1);
    msg->endpoint = ep;
    msg->msg = data;

    return msg;
}

static inline void apple_rtbuddy_send_msg(AppleRTBuddy *s, uint32_t ep,
                                          uint64_t data)
{
    apple_a7iop_send_ap(APPLE_A7IOP(s), apple_rtbuddy_construct_msg(ep, data));
}

void apple_rtbuddy_send_control_msg(AppleRTBuddy *s, uint32_t ep, uint64_t data)
{
    g_assert_cmpuint(ep, <, EP_USER_START);
    apple_rtbuddy_send_msg(s, ep, data);
}

void apple_rtbuddy_send_user_msg(AppleRTBuddy *s, uint32_t ep, uint64_t data)
{
    g_assert_cmpuint(ep, <, 224);
    apple_rtbuddy_send_msg(s, ep + EP_USER_START, data);
}

static inline void apple_rtbuddy_register_ep(AppleRTBuddy *s, uint32_t ep,
                                             void *opaque,
                                             AppleRTBuddyEPHandler *handler,
                                             bool user)
{
    AppleRTBuddyEPData *data;

    g_assert_nonnull(opaque);
    data = g_new0(AppleRTBuddyEPData, 1);
    data->opaque = opaque;
    data->handler = handler;
    data->user = user;
    g_tree_insert(s->endpoints, GUINT_TO_POINTER(ep), data);
}

void apple_rtbuddy_register_control_ep(AppleRTBuddy *s, uint32_t ep,
                                       void *opaque,
                                       AppleRTBuddyEPHandler *handler)
{
    g_assert_cmpuint(ep, <, EP_USER_START);
    apple_rtbuddy_register_ep(s, ep, opaque, handler, false);
}

void apple_rtbuddy_register_user_ep(AppleRTBuddy *s, uint32_t ep, void *opaque,
                                    AppleRTBuddyEPHandler *handler)
{
    g_assert_cmpuint(ep, <, 224);
    apple_rtbuddy_register_ep(s, ep + EP_USER_START, opaque, handler, true);
}

static inline void apple_rtbuddy_unregister_ep(AppleRTBuddy *s, uint32_t ep)
{
    void *ep_data = g_tree_lookup(s->endpoints, GUINT_TO_POINTER(ep));
    if (ep_data != NULL) {
        g_tree_remove(s->endpoints, GUINT_TO_POINTER(ep));
        g_free(ep_data);
    }
}

void apple_rtbuddy_unregister_control_ep(AppleRTBuddy *s, uint32_t ep)
{
    g_assert_cmpuint(ep, <, EP_USER_START);
    apple_rtbuddy_unregister_ep(s, ep);
}

void apple_rtbuddy_unregister_user_ep(AppleRTBuddy *s, uint32_t ep)
{
    g_assert_cmpuint(ep, <, 224);
    apple_rtbuddy_unregister_ep(s, ep + EP_USER_START);
}

static gboolean iop_rollcall(gpointer key, gpointer value, gpointer data)
{
    AppleRTBuddyRollcallData *d = (AppleRTBuddyRollcallData *)data;
    AppleRTBuddy *s = d->s;
    AppleRTBuddyManagementMessage mgmt_msg = { 0 };
    AppleA7IOPMessage *msg;

    uint32_t ep = (uint64_t)key;
    if ((uint64_t)key < 1) {
        return false;
    }

    if (ep / EP_USER_START != d->last_block && d->mask) {
        mgmt_msg.type = MSG_TYPE_ROLLCALL;
        mgmt_msg.rollcall.epMask = d->mask;
        mgmt_msg.rollcall.epBlock = d->last_block;
        mgmt_msg.rollcall.epEnded = false;
        msg = apple_rtbuddy_construct_msg(EP_MANAGEMENT, mgmt_msg.raw);
        QTAILQ_INSERT_TAIL(&s->rollcall, msg, entry);
        d->mask = 0;
    }
    d->last_block = ep / EP_USER_START;
    d->mask |= 1 << (ep & (EP_USER_START - 1));
    return false;
}

static void iop_start_rollcall(AppleRTBuddy *s)
{
    AppleA7IOP *a7iop;
    AppleRTBuddyRollcallData data = { 0 };
    AppleA7IOPMessage *msg;
    AppleRTBuddyManagementMessage mgmt_msg = { 0 };

    a7iop = APPLE_A7IOP(s);

    data.s = s;
    while (!QTAILQ_EMPTY(&s->rollcall)) {
        msg = QTAILQ_FIRST(&s->rollcall);
        QTAILQ_REMOVE(&s->rollcall, msg, entry);
        g_free(msg);
    }
    s->ep0_status = EP0_WAIT_ROLLCALL;
    g_tree_foreach(s->endpoints, iop_rollcall, &data);
    mgmt_msg.type = MSG_TYPE_ROLLCALL;
    mgmt_msg.rollcall.epMask = data.mask;
    mgmt_msg.rollcall.epBlock = data.last_block;
    mgmt_msg.rollcall.epEnded = true;
    msg = apple_rtbuddy_construct_msg(EP_MANAGEMENT, mgmt_msg.raw);
    QTAILQ_INSERT_TAIL(&s->rollcall, msg, entry);

    msg = QTAILQ_FIRST(&s->rollcall);
    QTAILQ_REMOVE(&s->rollcall, msg, entry);
    apple_a7iop_send_ap(a7iop, msg);
}

static void apple_rtbuddy_handle_mgmt_msg(void *opaque, uint32_t ep,
                                          uint64_t message)
{
    AppleRTBuddy *s;
    AppleA7IOP *a7iop;
    AppleRTBuddyManagementMessage *msg;
    AppleRTBuddyManagementMessage m = { 0 };

    s = APPLE_RTBUDDY(opaque);
    a7iop = APPLE_A7IOP(opaque);
    msg = (AppleRTBuddyManagementMessage *)&message;

    trace_apple_rtbuddy_handle_mgmt_msg(a7iop->role, msg->raw, s->ep0_status);

    switch (msg->type) {
    case MSG_TYPE_PING:
        m.type = MSG_TYPE_PING_ACK;
        m.ping.seg = msg->ping.seg;
        m.ping.timestamp = msg->ping.timestamp;
        apple_rtbuddy_send_msg(s, ep, m.raw);
        return;
    case MSG_TYPE_SET_AP_PSTATE:
        m.type = MSG_TYPE_SET_AP_PSTATE;
        m.power.state = msg->power.state;
        apple_rtbuddy_send_msg(s, ep, m.raw);
        return;
    default:
        break;
    }
    switch (s->ep0_status) {
    case EP0_IDLE:
        switch (msg->type) {
        case MSG_TYPE_SET_IOP_PSTATE: {
            switch (MSG_GET_PSTATE(msg->raw)) {
            case PSTATE_WAIT_VR:
                QEMU_FALLTHROUGH;
            case PSTATE_ON:
                apple_a7iop_cpu_start(a7iop, true);
                break;
            case PSTATE_SLPNOMEM:
                m.type = MSG_TYPE_SET_AP_PSTATE_ACK;
                m.power.state = MSG_GET_PSTATE(msg->raw);
                apple_a7iop_set_cpu_status(a7iop, CPU_STATUS_IDLE);
                apple_rtbuddy_send_msg(s, ep, m.raw);
                break;
            default:
                break;
            }
            break;
        }
        default:
            break;
        }
        break;
    case EP0_WAIT_HELLO:
        if (msg->type == MSG_RECV_HELLO) {
            iop_start_rollcall(s);
        }
        break;
    case EP0_WAIT_ROLLCALL:
        switch (msg->type) {
        case MSG_TYPE_ROLLCALL: {
            if (QTAILQ_EMPTY(&s->rollcall)) {
                m.type = MSG_TYPE_SET_AP_PSTATE_ACK;
                m.power.state = 32;
                s->ep0_status = EP0_IDLE;
                trace_apple_rtbuddy_rollcall_finished(a7iop->role);
                apple_rtbuddy_send_msg(s, ep, m.raw);
            } else {
                AppleA7IOPMessage *m2 = QTAILQ_FIRST(&s->rollcall);
                QTAILQ_REMOVE(&s->rollcall, m2, entry);
                apple_a7iop_send_ap(a7iop, m2);
            }
            break;
        }
        case MSG_TYPE_EPSTART: {
            break;
        }
        default:
            break;
        }
        break;
    default:
        break;
    }
}

static void apple_rtbuddy_mgmt_send_hello(AppleRTBuddy *s)
{
    AppleRTBuddyManagementMessage msg = { 0 };

    trace_apple_rtbuddy_mgmt_send_hello(APPLE_A7IOP(s)->role);

    msg.type = MSG_SEND_HELLO;
    msg.hello.major = s->protocol_version;
    msg.hello.minor = s->protocol_version;
    s->ep0_status = EP0_WAIT_HELLO;

    apple_rtbuddy_send_control_msg(s, EP_MANAGEMENT, msg.raw);
}

static void apple_rtbuddy_iop_start(AppleA7IOP *s)
{
    AppleRTBuddy *rtb;

    rtb = APPLE_RTBUDDY(s);

    trace_apple_rtbuddy_iop_start(s->role);

    apple_a7iop_set_cpu_status(s, apple_a7iop_get_cpu_status(s) &
                                      ~CPU_STATUS_IDLE);

    if (rtb->ops && rtb->ops->start) {
        rtb->ops->start(rtb->opaque);
    }

    apple_rtbuddy_mgmt_send_hello(rtb);
}

static void apple_rtbuddy_iop_wakeup(AppleA7IOP *s)
{
    AppleRTBuddy *rtb;

    rtb = APPLE_RTBUDDY(s);

    trace_apple_rtbuddy_iop_wakeup(s->role);

    apple_a7iop_set_cpu_status(s, apple_a7iop_get_cpu_status(s) &
                                      ~CPU_STATUS_IDLE);

    if (rtb->ops && rtb->ops->wakeup) {
        rtb->ops->wakeup(rtb->opaque);
    }

    apple_rtbuddy_mgmt_send_hello(rtb);
}

static void apple_rtbuddy_bh(void *opaque)
{
    AppleRTBuddy *s;
    AppleA7IOP *a7iop;
    AppleRTBuddyEPData *data;
    AppleA7IOPMessage *msg;

    s = APPLE_RTBUDDY(opaque);
    a7iop = APPLE_A7IOP(opaque);

    QEMU_LOCK_GUARD(&s->lock);
    while (!apple_a7iop_mailbox_is_empty(a7iop->iop_mailbox)) {
        msg = apple_a7iop_recv_iop(a7iop);
        data = g_tree_lookup(s->endpoints, GUINT_TO_POINTER(msg->endpoint));
        if (data && data->handler) {
            data->handler(data->opaque,
                          data->user ? msg->endpoint - EP_USER_START :
                                       msg->endpoint,
                          msg->msg);
        }
        g_free(msg);
    }
}

static const AppleA7IOPOps apple_rtbuddy_iop_ops = {
    .start = apple_rtbuddy_iop_start,
    .wakeup = apple_rtbuddy_iop_wakeup,
};

static gint g_uint_cmp(gconstpointer a, gconstpointer b)
{
    return a - b;
}

void apple_rtbuddy_init(AppleRTBuddy *s, void *opaque, const char *role,
                        uint64_t mmio_size, AppleA7IOPVersion version,
                        uint32_t protocol_version, const AppleRTBuddyOps *ops)
{
    AppleA7IOP *a7iop;

    a7iop = APPLE_A7IOP(s);
    apple_a7iop_init(a7iop, role, mmio_size, version, &apple_rtbuddy_iop_ops,
                     qemu_bh_new(apple_rtbuddy_bh, s));

    s->opaque = opaque ? opaque : s;
    s->endpoints = g_tree_new(g_uint_cmp);
    s->protocol_version = protocol_version;
    s->ops = ops;
    QTAILQ_INIT(&s->rollcall);
    qemu_mutex_init(&s->lock);
    apple_rtbuddy_register_control_ep(s, EP_MANAGEMENT, s,
                                      apple_rtbuddy_handle_mgmt_msg);
    apple_rtbuddy_register_control_ep(s, EP_CRASHLOG, s, NULL);
}

AppleRTBuddy *apple_rtbuddy_new(void *opaque, const char *role,
                                uint64_t mmio_size, AppleA7IOPVersion version,
                                uint32_t protocol_version,
                                const AppleRTBuddyOps *ops)
{
    AppleRTBuddy *s;

    s = APPLE_RTBUDDY(qdev_new(TYPE_APPLE_RTBUDDY));
    apple_rtbuddy_init(s, opaque, role, mmio_size, version, protocol_version,
                       ops);

    return s;
}

static void apple_rtbuddy_reset(Object *obj, ResetType type)
{
    AppleRTBuddy *s;
    AppleRTBuddyClass *rtbc;
    AppleA7IOPMessage *msg;

    s = APPLE_RTBUDDY(obj);
    rtbc = APPLE_RTBUDDY_GET_CLASS(obj);

    if (rtbc->parent_reset.hold != NULL) {
        rtbc->parent_reset.hold(obj, type);
    }

    QEMU_LOCK_GUARD(&s->lock);

    s->ep0_status = EP0_IDLE;

    while (!QTAILQ_EMPTY(&s->rollcall)) {
        msg = QTAILQ_FIRST(&s->rollcall);
        QTAILQ_REMOVE(&s->rollcall, msg, entry);
        g_free(msg);
    }
}

static void apple_rtbuddy_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc;
    DeviceClass *dc;
    AppleRTBuddyClass *rtbc;

    rc = RESETTABLE_CLASS(klass);
    dc = DEVICE_CLASS(klass);
    rtbc = APPLE_RTBUDDY_CLASS(klass);

    dc->desc = "Apple RTBuddy IOP";
    resettable_class_set_parent_phases(rc, NULL, apple_rtbuddy_reset, NULL,
                                       &rtbc->parent_reset);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo apple_rtbuddy_info = {
    .name = TYPE_APPLE_RTBUDDY,
    .parent = TYPE_APPLE_A7IOP,
    .instance_size = sizeof(AppleRTBuddy),
    .class_size = sizeof(AppleRTBuddyClass),
    .class_init = apple_rtbuddy_class_init,
};

static void apple_rtbuddy_register_types(void)
{
    type_register_static(&apple_rtbuddy_info);
}

type_init(apple_rtbuddy_register_types);
