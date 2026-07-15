#pragma once

// Minimal USB descriptor definitions for DRIVER MODULE builds (no IDF
// headers available). Wire-format packed structs + the handful of macros
// usb_drv_hid.cpp uses, mirroring IDF's usb/usb_types_ch9.h names so the
// same source compiles in firmware (real IDF types) and as a module (this
// shim). Descriptor layout is fixed by the USB 2.0 spec — nothing here can
// drift.

#include <stdint.h>

typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
} usb_standard_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_config_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} usb_intf_desc_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_ep_desc_t;

#define USB_B_DESCRIPTOR_TYPE_INTERFACE 4
#define USB_B_DESCRIPTOR_TYPE_ENDPOINT  5

#define USB_CLASS_HID 0x03

#define USB_TRANSFER_TYPE_INTR 3

#define USB_EP_DESC_GET_XFERTYPE(e) ((e)->bmAttributes & 0x03)
#define USB_EP_DESC_GET_EP_DIR(e)   (((e)->bEndpointAddress & 0x80) ? 1 : 0)
#define USB_EP_DESC_GET_MPS(e)      ((e)->wMaxPacketSize & 0x7FF)

// Walk to the next descriptor within the config blob — same call shape as
// IDF's usb_parse_next_descriptor (start with d = (usb_standard_desc_t*)cfg
// and offset = 0; NULL at the end).
static inline const usb_standard_desc_t*
usb_parse_next_descriptor(const usb_standard_desc_t* cur,
                          uint16_t wTotalLength, int* offset) {
    if (!cur || cur->bLength == 0) return 0;
    int next = *offset + cur->bLength;
    if (next >= (int)wTotalLength) return 0;
    *offset = next;
    return (const usb_standard_desc_t*)((const uint8_t*)cur + cur->bLength);
}
