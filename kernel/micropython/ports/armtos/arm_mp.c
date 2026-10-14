#include "py/compile.h"
#include "py/runtime.h"
#include "py/gc.h"
#include "py/stackctrl.h"
#include "py/mperrno.h"
#include "py/builtin.h"
#include "py/persistentcode.h"
#include "shared/runtime/pyexec.h"
#include "uart.h"
#include "heap.h"
#include <string.h>

/*
 * Starting and running the interpreter.
 *
 * MicroPython manages its own heap, so it is given one block from the kernel's and left to
 * it; that keeps a runaway script from eating the memory the drivers need.
 */

#define MP_HEAP_SIZE (2 * 1024 * 1024)

static char *mp_heap;
static int started;

int armpy_init(void)
{
    if (started) return 0;
    mp_heap = (char *)malloc(MP_HEAP_SIZE);
    if (!mp_heap) { uart_puts("python: not enough memory\n"); return -1; }

    mp_stack_ctrl_init();
    mp_stack_set_limit(16 * 1024);
    gc_init(mp_heap, mp_heap + MP_HEAP_SIZE);
    mp_init();
    started = 1;
    return 0;
}

/* Runs one line and prints whatever it evaluates to, the way a prompt does. */
int armpy_exec(const char *line)
{
    if (armpy_init() != 0) return -1;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, line, strlen(line), 0);
        qstr source_name = lex->source_name;
        mp_parse_tree_t tree = mp_parse(lex, MP_PARSE_SINGLE_INPUT);
        mp_obj_t module_fun = mp_compile(&tree, source_name, true);
        mp_call_function_0(module_fun);
        nlr_pop();
        return 0;
    }
    /* An error in a script is reported and the interpreter carries on, rather than taking
     * the board down with it. */
    mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
    return -1;
}

void armpy_repl(void)
{
    if (armpy_init() != 0) return;
    uart_puts("MicroPython on ArmtOS -- Ctrl-D to leave\n");
    pyexec_friendly_repl();
}

/* The runtime asks for these; there is no filesystem behind the interpreter yet, so an
 * import of a .py file is reported as missing rather than silently doing nothing. */
mp_import_stat_t mp_import_stat(const char *path) { (void)path; return MP_IMPORT_STAT_NO_EXIST; }

mp_obj_t mp_builtin_open(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs)
{
    (void)n_args; (void)args; (void)kwargs;
    mp_raise_OSError(MP_ENOENT);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, mp_builtin_open);

void nlr_jump_fail(void *val)
{
    (void)val;
    uart_puts("\npython: unrecoverable error\n");
    for (;;) { }
}

void NORETURN __fatal_error(const char *msg)
{
    uart_puts("\npython: ");
    uart_puts(msg);
    uart_puts("\n");
    for (;;) { }
}

/* ---- the few libc and runtime symbols MicroPython expects from its host ---- */

/* __assert_fail lives in the port's shared math_stubs.c, which this build also uses. */

/* There is no filesystem behind the interpreter yet, so importing a module from a file is
 * reported as missing. */
mp_lexer_t *mp_lexer_new_from_file(qstr filename)
{
    (void)filename;
    mp_raise_OSError(MP_ENOENT);
}

/* ---- what a hosted build would get from libc and the port ---- */

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* The collector needs to know which roots live on the stack; with one thread and no
 * scheduler, that is simply the range from the current stack pointer to where the
 * interpreter was entered. */
void gc_collect(void)
{
    gc_collect_start();
    uintptr_t sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    extern char _start[];
    uintptr_t top = (uintptr_t)_start;              /* the kernel stack grows down from here */
    if (sp < top) gc_collect_root((void **)sp, (top - sp) / sizeof(uintptr_t));
    gc_collect_end();
}
