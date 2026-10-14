#ifndef MICROPY_CONFIG_PORT_H
#define MICROPY_CONFIG_PORT_H

#include <alloca.h>

#define PATH_MAX 128

#define MICROPY_ALLOC_PATH_MAX      (PATH_MAX)
#define MICROPY_ENABLE_GC           1
#define MICROPY_HELPER_REPL         1
#define MICROPY_KBD_EXCEPTION       1
#define MICROPY_USE_INTERNAL_PRINTF 0
#define MICROPY_USE_INTERNAL_ERRNO  0
#define MICROPY_ERROR_REPORTING     MICROPY_ERROR_REPORTING_DETAILED
#define MICROPY_FLOAT_IMPL          MICROPY_FLOAT_IMPL_FLOAT
#define MICROPY_LONGINT_IMPL        MICROPY_LONGINT_IMPL_MPZ
#define MICROPY_PY_SYS_PLATFORM     "tOS"
#define MICROPY_PLATFORM_COMPILER   "GCC 14.2.0"
#define MICROPY_FLOAT_USE_NATIVE_FLT16 0
#define MICROPY_PY_SYS_STDFILES     1

#define MICROPY_PY___FILE__         0
#define MICROPY_PY_SYS_EXIT         0
#define MICROPY_PY_IO               0
#define MICROPY_PY_BUILTINS_IO_OPEN 1  /* tos_stubs.c's own open()/file object, not the full io module */
#define MICROPY_PY_IO_FILEIO        0
#define MICROPY_PY_OS               0
#define MICROPY_PY_THREAD           0
#define MICROPY_PY_FFI              0
#define MICROPY_PY_SOCKET           0
#define MICROPY_PY_NETWORK          0
#define MICROPY_PY_USSL             0
#define MICROPY_PY_WEBSOCKET        0
#define MICROPY_PY_BTREE            0
#define MICROPY_PY_CRYPTOLIB        0
#define MICROPY_PY_CMATH            0
#define MICROPY_PY_UCTYPE           0
#define MICROPY_PY_UZLIB            0
#define MICROPY_PY_UJSON            0
#define MICROPY_PY_URE              1
#define MICROPY_PY_UHEAPQ           0
#define MICROPY_PY_ARRAY            1
#define MICROPY_PY_COLLECTIONS      1
#define MICROPY_PY_MICROPYTHON_MEM_INFO 1

/* The interpreter's integer must be the size of a pointer, which differs between the two
 * builds: 32 bits on the x86 kernel, 64 on the AArch64 one. */
#if defined(__aarch64__)
typedef long mp_int_t;
#else
typedef int mp_int_t;
#endif
#if defined(__aarch64__)
typedef unsigned long mp_uint_t;
#else
typedef unsigned int mp_uint_t;
#endif
typedef long mp_off_t;

#define MP_STATE_PORT MP_STATE_VM

#define MICROPY_PORT_BUILTIN_MODULES \
    MICROPY_REGISTER_MODULE("math", mod_math) \
    MICROPY_REGISTER_MODULE("random", urandom_mod_info) \
    MICROPY_REGISTER_MODULE("struct", mod_struct_info) \
    MICROPY_REGISTER_MODULE("ubinascii", mod_ubinascii_info) \
    MICROPY_REGISTER_MODULE("ure", mod_ure_info) \
    MICROPY_REGISTER_MODULE("array", mod_array_info) \
    MICROPY_REGISTER_MODULE("collections", mod_collections_info)

#endif
