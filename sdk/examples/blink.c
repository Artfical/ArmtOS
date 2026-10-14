/* Blinks an LED on GPIO 21 and fades another on GPIO 18, then reads a button on GPIO 20. */
#include "armtos.h"

void main_(void)
{
    char board[64];
    board_name(board, sizeof(board));
    print("blink on ");
    print(board);
    print("\n");

    pin_mode(21, OUTPUT);
    pin_mode(20, INPUT);
    pin_pull(20, PULL_UP);          /* the button pulls the pin down when pressed */

    for (int i = 0; i < 5; i++) {
        pin_write(21, HIGH);
        delay_ms(200);
        pin_write(21, LOW);
        delay_ms(200);
        print("button: ");
        print(pin_read(20) ? "up\n" : "pressed\n");
    }

    /* a slow fade, which is what PWM is for: the pin is still only ever high or low */
    for (int duty = 0; duty <= 1023; duty += 64) {
        analog_write(18, duty);
        delay_ms(50);
    }
    analog_write(18, 0);

    print("done after ");
    print_num((long)millis());
    print(" ms\n");
}
