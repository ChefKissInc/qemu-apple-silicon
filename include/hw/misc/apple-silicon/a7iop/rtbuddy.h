#ifndef HW_MISC_APPLE_SILICON_A7IOP_RTKIT_H
#define HW_MISC_APPLE_SILICON_A7IOP_RTKIT_H

#include "qemu/osdep.h"
#include "hw/misc/apple-silicon/a7iop/core.h"
#include "qemu/queue.h"

#define TYPE_APPLE_RTBUDDY "apple-rtbuddy"
OBJECT_DECLARE_TYPE(AppleRTBuddy, AppleRTBuddyClass, APPLE_RTBUDDY)

#define EP_MANAGEMENT 0
#define EP_CRASHLOG 1
#define EP_USER_START 32

typedef enum {
    EP0_IDLE,
    EP0_WAIT_HELLO,
    EP0_WAIT_ROLLCALL,
    EP0_DONE,
} AppleRTBuddyEP0State;

typedef struct QEMU_PACKED {
    union {
        uint64_t raw;
        struct QEMU_PACKED {
            union {
                struct QEMU_PACKED {
                    uint16_t major;
                    uint16_t minor;
                } hello;
                struct QEMU_PACKED {
                    uint32_t seg;
                    uint16_t timestamp;
                } ping;
                struct QEMU_PACKED {
                    uint32_t state;
                    uint32_t ep;
                } epstart;
                struct QEMU_PACKED {
                    uint32_t state;
                } power;
                struct QEMU_PACKED {
                    uint32_t epMask;
                    /* bit x -> endpoint ((epBlock * 32) + x) */
                    uint8_t epBlock : 6;
                    uint16_t unk38 : 13;
                    uint8_t epEnded : 1;
                } rollcall;
            };
        };
        struct QEMU_PACKED {
            uint32_t field_0;
            uint16_t field_32;
            uint8_t field_48 : 4;
            uint8_t type : 4;
        };
    };
} AppleRTBuddyManagementMessage;

typedef void AppleRTBuddyEPHandler(void *opaque, uint32_t ep, uint64_t msg);

typedef struct {
    void *opaque;
    AppleRTBuddyEPHandler *handler;
    bool user;
} AppleRTBuddyEPData;

typedef struct {
    AppleRTBuddy *s;
    uint32_t mask;
    uint32_t last_block;
} AppleRTBuddyRollcallData;

typedef struct {
    void (*start)(void *opaque);
    void (*wakeup)(void *opaque);
} AppleRTBuddyOps;

struct AppleRTBuddyClass {
    /*< private >*/
    SysBusDevice base_class;

    /*< public >*/
    ResettablePhases parent_reset;
};

struct AppleRTBuddy {
    /*< private >*/
    AppleA7IOP parent_obj;

    /*< public >*/
    const AppleRTBuddyOps *ops;
    QemuMutex lock;
    void *opaque;
    AppleRTBuddyEP0State ep0_status;
    uint32_t protocol_version;
    GTree *endpoints;
    QTAILQ_HEAD(, AppleA7IOPMessage) rollcall;
};

void apple_rtbuddy_send_control_msg(AppleRTBuddy *s, uint32_t ep,
                                    uint64_t data);
void apple_rtbuddy_send_user_msg(AppleRTBuddy *s, uint32_t ep, uint64_t data);
void apple_rtbuddy_register_control_ep(AppleRTBuddy *s, uint32_t ep,
                                       void *opaque,
                                       AppleRTBuddyEPHandler *handler);
void apple_rtbuddy_register_user_ep(AppleRTBuddy *s, uint32_t ep, void *opaque,
                                    AppleRTBuddyEPHandler *handler);
void apple_rtbuddy_unregister_control_ep(AppleRTBuddy *s, uint32_t ep);
void apple_rtbuddy_unregister_user_ep(AppleRTBuddy *s, uint32_t ep);
void apple_rtbuddy_init(AppleRTBuddy *s, void *opaque, const char *role,
                        uint64_t mmio_size, AppleA7IOPVersion version,
                        uint32_t protocol_version, const AppleRTBuddyOps *ops);
AppleRTBuddy *apple_rtbuddy_new(void *opaque, const char *role,
                                uint64_t mmio_size, AppleA7IOPVersion version,
                                uint32_t protocol_version,
                                const AppleRTBuddyOps *ops);

#endif /* HW_MISC_APPLE_SILICON_A7IOP_RTKIT_H */
