/**
 * usbd_storage.c — USB MSC storage callbacks (thin shim → usb_msc.c)
 *
 * Does NOT include audio_sd.h. All SD logic is in usb_msc.c.
 */

#include "usbd_storage.h"
#include "usb_msc.h"

/* MSC Inquiry data */
static int8_t s_inquiry[] = {
    0x00, 0x80, 0x02, 0x02,
    (STANDARD_INQUIRY_DATA_LEN - 5),
    0x00, 0x00, 0x00,
    'N', 'O', 'R', 'A', ' ', ' ', ' ', ' ',
    'V', 'o', 'i', 'c', 'e', 'R', 'e', 'c',
    ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
    '1', '.', '0', '0',
};

USBD_StorageTypeDef USBD_DISK_fops = {
    USB_MSC_StorageInit,
    USB_MSC_StorageGetCapacity,
    USB_MSC_StorageIsReady,
    USB_MSC_StorageIsWriteProtected,
    USB_MSC_StorageRead,
    USB_MSC_StorageWrite,
    USB_MSC_StorageGetMaxLun,
    s_inquiry,
};
