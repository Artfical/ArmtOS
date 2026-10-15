#ifndef ARM_ETHDEV_H
#define ARM_ETHDEV_H

#include <stdint.h>

/*
 * Ethernet.
 *
 * Which chip is behind the socket depends on the board: a Pi 3 has a SMSC LAN9514 on the
 * USB bus, a Pi 4 the GENET block wired straight into memory, and a Pi 5 puts it behind
 * RP1. One interface covers them, and the right driver is chosen at boot.
 */

#define ETH_MTU 1536

typedef struct {
    const char *name;
    int  (*start)(void);
    int  (*send)(const uint8_t *frame, int len);
    int  (*recv)(uint8_t *frame, int max);     /* 0 when nothing is waiting */
    uint8_t mac[6];
} eth_driver_t;

int  eth_init(void);                      /* finds and starts whatever this board has */
int  eth_send(const uint8_t *frame, int len);
int  eth_recv(uint8_t *frame, int max);
const uint8_t *eth_mac(void);
const char *eth_name(void);               /* which driver is in use, or NULL */

#endif
