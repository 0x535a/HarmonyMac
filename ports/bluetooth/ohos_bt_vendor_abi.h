/* ABI checked against drivers_peripheral 8cd47d4, bluetooth/hci/hdi_service/
 * implement/ohos_bt_vendor_lib.h. No L410 controller-specific behavior. */
#ifndef AURORA_OHOS_BT_VENDOR_ABI_H
#define AURORA_OHOS_BT_VENDOR_ABI_H
#include <stddef.h>
#include <stdint.h>
typedef enum { HCI_CMD,HCI_EVT,HCI_ACL_OUT,HCI_ACL_IN,HCI_MAX_CHANNEL } hci_channels_t;
typedef enum { BTC_OP_RESULT_SUCCESS,BTC_OP_RESULT_FAIL } BtOpResultT;
typedef enum {
    BT_OP_POWER_ON,BT_OP_POWER_OFF,BT_OP_HCI_CHANNEL_OPEN,BT_OP_HCI_CHANNEL_CLOSE,
    BT_OP_INIT,BT_OP_GET_LPM_TIMER,BT_OP_LPM_ENABLE,BT_OP_LPM_DISABLE,
    BT_OP_WAKEUP_LOCK,BT_OP_WAKEUP_UNLOCK,BT_OP_EVENT_CALLBACK
} BtOpcodeT;
typedef struct {
    size_t size;
    void (*initCb)(BtOpResultT);
    void *(*alloc)(int);
    void (*dealloc)(void *);
    size_t (*xmitCb)(uint16_t,void *);
} BtVendorCallbacksT;
typedef struct {
    size_t size;
    int (*init)(const BtVendorCallbacksT *,unsigned char *);
    int (*op)(BtOpcodeT,void *);
    void (*close)(void);
} BtVendorInterfaceT;
#endif
