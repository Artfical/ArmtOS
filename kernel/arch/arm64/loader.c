#include "loader.h"
#include "mmu.h"
#include "uart.h"
#include "string_arm.h"

void arm_enter_user(uint64_t entry, uint64_t stack_top);

void arm_user_return(void);

/* There is one program at a time and its end simply makes arm_enter_user return, with the
 * caller's callee-saved registers and stack restored (see exception.S). */
static int running;

int loader_run(const uint8_t *image, uint32_t len)
{
    if (len <= 4) { uart_puts("run: not a program\n"); return -1; }
    uint32_t content = len - 4;
    uint32_t mem = (uint32_t)image[content] | ((uint32_t)image[content + 1] << 8) |
                   ((uint32_t)image[content + 2] << 16) | ((uint32_t)image[content + 3] << 24);
    if (mem < content) mem = content;
    if (mem > USER_REGION - 65536) { uart_puts("run: program wants more memory than a program may have\n"); return -1; }

    uint8_t *dst = (uint8_t *)USER_LOAD_ADDR;
    memcpy(dst, image, content);
    memset(dst + content, 0, mem - content);     /* the .bss is not in the file */

    mmu_set_user(USER_LOAD_ADDR, USER_REGION);

    running = 1;
    arm_enter_user(USER_LOAD_ADDR, USER_STACK_TOP);
    running = 0;
    return 0;
}

/* Called from the exception path when the program exits or does something it may not. */
void loader_return(void)
{
    if (!running) return;
    running = 0;
    arm_user_return();
}

int loader_running(void) { return running; }
