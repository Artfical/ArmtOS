#ifndef DWC2_H
#define DWC2_H

#include <stdint.h>

/*
 * The Synopsys DWC2 USB 2.0 host controller: the only USB on a Pi 3 and earlier, and on
 * those boards the Ethernet port hangs off it too, so nothing plugs in without this.
 *
 * Only host mode is implemented, and only the control and bulk transfers a mass-storage
 * or network device needs -- there is no interrupt or isochronous scheduling here.
 */

#define USB_MAX_DEVICES 8

typedef struct {
    int      used;
    uint8_t  address;          /* what the device was assigned */
    uint8_t  speed;            /* 0 high, 1 full, 2 low */
    uint16_t vendor, product;
    uint8_t  dev_class, dev_subclass, dev_protocol;
    uint8_t  config_value;
    int      hub_port;
} usb_device_t;

int  dwc2_init(void);                      /* 0 = the controller came up */
int  usb_enumerate(void);                  /* finds what is plugged in; returns the count */
int  usb_device_count(void);
const usb_device_t *usb_device_get(int i);

/* One control transfer on the default pipe. `data` may be NULL for a no-data request. */
int  usb_control(usb_device_t *dev, uint8_t rtype, uint8_t req, uint16_t value,
                 uint16_t index, void *data, uint16_t len);
/* One bulk transfer; `ep` carries the direction bit as the descriptor reports it. */
int  usb_bulk(usb_device_t *dev, uint8_t ep, void *data, int len);

/* A receive that never blocks: 0 when nothing has arrived, the byte count when something has,
 * -1 on an error. One transfer stays armed between calls. */
int  usb_bulk_in_poll(usb_device_t *dev, uint8_t ep, void *data, int max);

/* Descriptor helpers for class drivers: the device's full configuration descriptor, and a
 * string descriptor decoded to ASCII. */
int  usb_get_config(usb_device_t *dev, int index, uint8_t *buf, int max);
int  usb_get_string(usb_device_t *dev, int index, char *out, int max);
int  usb_set_config(usb_device_t *dev, int value);
int  usb_set_interface(usb_device_t *dev, int iface, int alt);

#endif
