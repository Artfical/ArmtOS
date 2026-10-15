#include <stdint.h>
#include "nic.h"
#include "ethdev.h"
#include "string_arm.h"

/*
 * The x86 kernel probes PCI for a network card (kernel/net/nic.c). On the board the card is
 * whichever driver ethdev.c settled on, so the same function pointers the stack calls are
 * simply pointed at it.
 */

uint32_t nic_rx_packets, nic_tx_packets, nic_rx_bytes, nic_tx_bytes;
char     nic_driver_name[32] = "None";
void   (*nic_send)(void *data, int len);
int    (*nic_poll)(uint8_t *buf, int max_len);

static void arm_send(void *data, int len)
{
    nic_tx_packets++;
    nic_tx_bytes += (uint32_t)len;
    eth_send((const uint8_t *)data, len);
}

static int arm_poll(uint8_t *buf, int max_len)
{
    int n = eth_recv(buf, max_len);
    if (n > 0) { nic_rx_packets++; nic_rx_bytes += (uint32_t)n; }
    return n;
}

void nic_transmit(void *data, int len)
{
    if (!nic_send) return;
    if (len >= 60) { nic_send(data, len); return; }
    uint8_t buf[60];
    memcpy(buf, data, (size_t)len);
    memset(buf + len, 0, (size_t)(60 - len));
    nic_send(buf, 60);
}

extern uint8_t net_mac[6];

int nic_init(void)
{
    if (nic_send) return 0;                       /* already up */
    if (eth_init() != 0) return -1;
    const char *n = eth_name();
    int i = 0;
    while (n && n[i] && i < 31) { nic_driver_name[i] = n[i]; i++; }
    nic_driver_name[i] = 0;
    memcpy(net_mac, eth_mac(), 6);
    nic_send = arm_send;
    nic_poll = arm_poll;
    return 0;
}
