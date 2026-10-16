#ifndef ARM_USBHID_H
#define ARM_USBHID_H

/* USB mouse and keyboard (HID boot protocol), polled. */

#define USBHID_KEY_UP    0x11
#define USBHID_KEY_DOWN  0x12
#define USBHID_KEY_LEFT  0x13
#define USBHID_KEY_RIGHT 0x14

int  usbhid_init(void);              /* after usb_enumerate(); 0 = something was found */
void usbhid_poll(void);
void usbhid_set_screen(int w, int h);
void usbhid_pointer(int *x, int *y, int *buttons);
int  usbhid_wheel(void);
int  usbhid_getc(void);              /* -1 when nothing was typed */
int  usbhid_has_mouse(void);
int  usbhid_has_keyboard(void);

#endif
