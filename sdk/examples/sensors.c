/* Lists what is on the I2C bus and reads an analog channel through an external ADC. */
#include "armtos.h"

void main_(void)
{
    i2c_setup(100000);
    u8 found[16];
    int n = i2c_scan(found, 16);
    print("I2C devices: ");
    print_num(n);
    print("\n");
    for (int i = 0; i < n; i++) {
        print("  address ");
        print_num(found[i]);
        print("\n");
    }

    /* The Pi cannot measure a voltage itself, so this reads an MCP3008 on SPI. */
    spi_setup(1000000);
    for (int ch = 0; ch < 4; ch++) {
        int v = analog_read(ch);
        print("  channel ");
        print_num(ch);
        print(" = ");
        if (v < 0) print("no ADC\n");
        else { print_num(v); print(" of 1023\n"); }
    }
}
