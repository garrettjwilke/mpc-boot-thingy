#pragma once
#include <stddef.h>

enum {
    USB_STORAGE_STATUS_IDLE = 0,
    USB_STORAGE_STATUS_PREPARING,
    USB_STORAGE_STATUS_ACTIVE,
    USB_STORAGE_STATUS_EXITING,
    USB_STORAGE_STATUS_ERROR
};

/* Finds external SD card block device (e.g. /dev/mmcblk1).
   Returns 0 on success with device path in out, -1 on failure. */
int usb_storage_find_sd_device(char *out, size_t n);

/* Starts USB mass storage mode:
   1. Unmounts active /media/ partitions belonging to SD card.
   2. Invokes 'az0x-usb-gadget switch sd-access'.
   3. Polls configfs mass_storage lun.0/file.
   4. Writes SD block device path into lun.0/file.
   Returns 0 on success, negative error code on failure with err_buf set. */
int usb_storage_start(char *err_buf, size_t err_len);

/* Stops USB mass storage mode:
   1. Flushes filesystem buffers (sync).
   2. Clears lun.0/file (unbinding media).
   3. Invokes 'az0x-usb-gadget switch' to restore default composite gadget.
   4. Drops kernel caches and triggers remount.
   Returns 0 on success, negative error code on failure. */
int usb_storage_stop(char *err_buf, size_t err_len);

/* Test mock control for host unit tests */
void usb_storage_set_mock(int enable, int fail_start);
int usb_storage_is_mock(void);
