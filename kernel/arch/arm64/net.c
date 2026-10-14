#include "net.h"
#include "bcm.h"
#include "uart.h"
#include "dwc2.h"
#include "string_arm.h"

extern eth_driver_t genet_driver;
extern eth_driver_t lan95xx_driver;
void genet_publish_mac(uint8_t *out);
void lan95xx_publish_mac(uint8_t *out);

static eth_driver_t *active;
static uint8_t mac[6];

int eth_init(void)
{
    active = 0;

    /* On a Pi 4 the controller is in the SoC; trying it first costs one register read and
     * answers immediately on a board that does not have it. */
    if (bcm_generation >= 4 && genet_driver.start() == 0) {
        active = &genet_driver;
        genet_publish_mac(mac);
    } else {
        /* Otherwise the Ethernet is a USB device, so the bus has to be walked first. */
        if (dwc2_init() == 0) {
            usb_enumerate();
            if (lan95xx_driver.start() == 0) {
                active = &lan95xx_driver;
                lan95xx_publish_mac(mac);
            }
        }
    }
    if (!active) return -1;
    memcpy(active->mac, mac, 6);
    return 0;
}

int eth_send(const uint8_t *frame, int len) { return active ? active->send(frame, len) : -1; }
int eth_recv(uint8_t *frame, int max)       { return active ? active->recv(frame, max) : 0; }
const uint8_t *eth_mac(void)                { return mac; }
const char *eth_name(void)                  { return active ? active->name : 0; }
