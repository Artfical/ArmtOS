/* Draws on the display the firmware hands over. */
#include "armtos.h"

void main_(void)
{
    if (fb_setup(640, 480) != 0) { print("no display\n"); return; }
    int w = fb_width(), h = fb_height();

    fb_rect(0, 0, w, h, 0x00101828);
    fb_text(20, 20, "ArmtOS", 0x00FFFFFF);

    char board[64];
    board_name(board, sizeof(board));
    fb_text(20, 40, board, 0x0080C0FF);

    /* a gradient, one column at a time */
    for (int x = 0; x < w; x++) {
        u32 c = (u32)((x * 255 / w) << 8);
        fb_rect(x, h - 60, 1, 40, c);
    }
    print("drawn\n");
}
