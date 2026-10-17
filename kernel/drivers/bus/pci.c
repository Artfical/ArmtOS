#include "pci.h"
#include "io.h"

uint32_t pci_read_config(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset)
{
    uint32_t address = (uint32_t)((bus << 16) | (device << 11) | (func << 8) | (offset & 0xFC) | 0x80000000);
    outl(PCI_CONFIG_ADDR, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config(uint8_t bus, uint8_t device, uint8_t func, uint8_t offset, uint32_t value)
{
    uint32_t address = (uint32_t)((bus << 16) | (device << 11) | (func << 8) | (offset & 0xFC) | 0x80000000);
    outl(PCI_CONFIG_ADDR, address);
    outl(PCI_CONFIG_DATA, value);
}

int pci_find_devices(uint8_t class_code, uint8_t subclass, pci_device_t *devices, int max_devices)
{
    int count = 0;
    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            for (int func = 0; func < 8; func++) {
                uint32_t vendor_device = pci_read_config(bus, dev, func, 0);
                uint16_t vendor_id = vendor_device & 0xFFFF;
                if (vendor_id == 0xFFFF) {
                    if (func == 0) break;
                    continue;
                }

                uint32_t class_rev = pci_read_config(bus, dev, func, 8);
                uint8_t dev_class = (class_rev >> 24) & 0xFF;
                uint8_t dev_subclass = (class_rev >> 16) & 0xFF;

                if (dev_class == class_code && dev_subclass == subclass) {
                    if (count < max_devices) {
                        devices[count].vendor_id = vendor_id;
                        devices[count].device_id = (vendor_device >> 16) & 0xFFFF;
                        devices[count].class_code = dev_class;
                        devices[count].subclass = dev_subclass;
                        devices[count].prog_if = (class_rev >> 8) & 0xFF;
                        devices[count].rev_id = class_rev & 0xFF;
                        devices[count].bus = bus;
                        devices[count].device = dev;
                        devices[count].func = func;
                        count++;
                    }
                }

                /* bit 7 of the header type (offset 0x0E) says the device has more functions */
                if (func == 0 && !(pci_read_config(bus, dev, 0, 0x0C) & 0x800000))
                    break;
            }
        }
    }
    return count;
}

uint32_t pci_get_bar(uint8_t bus, uint8_t device, uint8_t func, int bar_num)
{
    return pci_read_config(bus, device, func, 0x10 + bar_num * 4);
}
