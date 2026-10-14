#include "py/runtime.h"
#include "py/objstr.h"
#include "hwapi.h"

/*
 * The `pi` module: every piece of the board's hardware, from Python.
 *
 *   import pi
 *   pi.pin_mode(21, pi.OUTPUT)
 *   pi.write(21, 1)
 *   pi.analog_write(18, 512)       # PWM, the closest thing to an analog output
 *   print(pi.analog_read(0))       # an MCP3008 on SPI, since the Pi cannot measure one
 *   print(pi.i2c_scan())
 *
 * It is a thin wrapper over the same hwapi the shell and `.t` programs use, so all three
 * behave identically and a board quirk is fixed in one place.
 */

static mp_obj_t pi_pin_mode(mp_obj_t pin, mp_obj_t mode)
{
    return mp_obj_new_int(hw_pin_mode(mp_obj_get_int(pin), mp_obj_get_int(mode)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(pi_pin_mode_obj, pi_pin_mode);

static mp_obj_t pi_pull(mp_obj_t pin, mp_obj_t pull)
{
    return mp_obj_new_int(hw_pin_pull(mp_obj_get_int(pin), mp_obj_get_int(pull)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(pi_pull_obj, pi_pull);

static mp_obj_t pi_write(mp_obj_t pin, mp_obj_t value)
{
    return mp_obj_new_int(hw_pin_write(mp_obj_get_int(pin), mp_obj_get_int(value)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(pi_write_obj, pi_write);

static mp_obj_t pi_read(mp_obj_t pin) { return mp_obj_new_int(hw_pin_read(mp_obj_get_int(pin))); }
static MP_DEFINE_CONST_FUN_OBJ_1(pi_read_obj, pi_read);

static mp_obj_t pi_analog_write(mp_obj_t pin, mp_obj_t value)
{
    return mp_obj_new_int(hw_analog_write(mp_obj_get_int(pin), mp_obj_get_int(value)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(pi_analog_write_obj, pi_analog_write);

static mp_obj_t pi_analog_read(mp_obj_t ch) { return mp_obj_new_int(hw_analog_read(mp_obj_get_int(ch))); }
static MP_DEFINE_CONST_FUN_OBJ_1(pi_analog_read_obj, pi_analog_read);

static mp_obj_t pi_i2c_setup(size_t n, const mp_obj_t *args)
{
    return mp_obj_new_int(hw_i2c_setup(n ? mp_obj_get_int(args[0]) : 100000));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pi_i2c_setup_obj, 0, 1, pi_i2c_setup);

/* Returns the addresses that answered, as a list -- which is what a caller wants to loop
 * over, rather than a buffer to pick apart. */
static mp_obj_t pi_i2c_scan(void)
{
    uint8_t found[32];
    int n = hw_i2c_scan(found, (int)sizeof(found));
    mp_obj_t list = mp_obj_new_list(0, NULL);
    for (int i = 0; i < n; i++) mp_obj_list_append(list, mp_obj_new_int(found[i]));
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_0(pi_i2c_scan_obj, pi_i2c_scan);

static mp_obj_t pi_i2c_read(mp_obj_t addr, mp_obj_t count)
{
    uint8_t buf[64];
    int n = mp_obj_get_int(count);
    if (n < 0 || n > (int)sizeof(buf)) mp_raise_ValueError(MP_ERROR_TEXT("too many bytes"));
    int got = hw_i2c_read(mp_obj_get_int(addr), buf, n);
    if (got < 0) return mp_const_none;
    return mp_obj_new_bytes(buf, (size_t)got);
}
static MP_DEFINE_CONST_FUN_OBJ_2(pi_i2c_read_obj, pi_i2c_read);

static mp_obj_t pi_i2c_write(mp_obj_t addr, mp_obj_t data)
{
    mp_buffer_info_t b;
    mp_get_buffer_raise(data, &b, MP_BUFFER_READ);
    return mp_obj_new_int(hw_i2c_write(mp_obj_get_int(addr), (const uint8_t *)b.buf, (int)b.len));
}
static MP_DEFINE_CONST_FUN_OBJ_2(pi_i2c_write_obj, pi_i2c_write);

static mp_obj_t pi_i2c_read_reg(size_t n, const mp_obj_t *args)
{
    uint8_t buf[64];
    int count = n > 2 ? mp_obj_get_int(args[2]) : 1;
    if (count < 0 || count > (int)sizeof(buf)) mp_raise_ValueError(MP_ERROR_TEXT("too many bytes"));
    int got = hw_i2c_read_reg(mp_obj_get_int(args[0]), mp_obj_get_int(args[1]), buf, count);
    if (got < 0) return mp_const_none;
    return mp_obj_new_bytes(buf, (size_t)got);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pi_i2c_read_reg_obj, 2, 3, pi_i2c_read_reg);

static mp_obj_t pi_spi_setup(size_t n, const mp_obj_t *args)
{
    return mp_obj_new_int(hw_spi_setup(n ? mp_obj_get_int(args[0]) : 1000000));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pi_spi_setup_obj, 0, 1, pi_spi_setup);

static mp_obj_t pi_spi_transfer(mp_obj_t cs, mp_obj_t data)
{
    mp_buffer_info_t b;
    mp_get_buffer_raise(data, &b, MP_BUFFER_READ);
    uint8_t rx[64];
    if (b.len > sizeof(rx)) mp_raise_ValueError(MP_ERROR_TEXT("too many bytes"));
    int got = hw_spi_transfer(mp_obj_get_int(cs), (const uint8_t *)b.buf, rx, (int)b.len);
    if (got < 0) return mp_const_none;
    return mp_obj_new_bytes(rx, (size_t)got);
}
static MP_DEFINE_CONST_FUN_OBJ_2(pi_spi_transfer_obj, pi_spi_transfer);

static mp_obj_t pi_board(void) { return mp_obj_new_str(hw_board_name(), strlen(hw_board_name())); }
static MP_DEFINE_CONST_FUN_OBJ_0(pi_board_obj, pi_board);

static mp_obj_t pi_micros(void) { return mp_obj_new_int_from_ull(hw_micros()); }
static MP_DEFINE_CONST_FUN_OBJ_0(pi_micros_obj, pi_micros);

static mp_obj_t pi_fb_setup(mp_obj_t w, mp_obj_t h)
{
    return mp_obj_new_int(hw_fb_setup(mp_obj_get_int(w), mp_obj_get_int(h)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(pi_fb_setup_obj, pi_fb_setup);

static mp_obj_t pi_fb_rect(size_t n, const mp_obj_t *args)
{
    (void)n;
    hw_fb_rect(mp_obj_get_int(args[0]), mp_obj_get_int(args[1]), mp_obj_get_int(args[2]),
               mp_obj_get_int(args[3]), (uint32_t)mp_obj_get_int(args[4]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pi_fb_rect_obj, 5, 5, pi_fb_rect);

static mp_obj_t pi_fb_text(size_t n, const mp_obj_t *args)
{
    (void)n;
    hw_fb_text(mp_obj_get_int(args[0]), mp_obj_get_int(args[1]),
               mp_obj_str_get_str(args[2]), (uint32_t)mp_obj_get_int(args[3]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pi_fb_text_obj, 4, 4, pi_fb_text);

static const mp_rom_map_elem_t pi_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),     MP_ROM_QSTR(MP_QSTR_pi) },
    { MP_ROM_QSTR(MP_QSTR_pin_mode),     MP_ROM_PTR(&pi_pin_mode_obj) },
    { MP_ROM_QSTR(MP_QSTR_pull),         MP_ROM_PTR(&pi_pull_obj) },
    { MP_ROM_QSTR(MP_QSTR_write),        MP_ROM_PTR(&pi_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_read),         MP_ROM_PTR(&pi_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_analog_write), MP_ROM_PTR(&pi_analog_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_analog_read),  MP_ROM_PTR(&pi_analog_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_i2c_setup),    MP_ROM_PTR(&pi_i2c_setup_obj) },
    { MP_ROM_QSTR(MP_QSTR_i2c_scan),     MP_ROM_PTR(&pi_i2c_scan_obj) },
    { MP_ROM_QSTR(MP_QSTR_i2c_read),     MP_ROM_PTR(&pi_i2c_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_i2c_write),    MP_ROM_PTR(&pi_i2c_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_i2c_read_reg), MP_ROM_PTR(&pi_i2c_read_reg_obj) },
    { MP_ROM_QSTR(MP_QSTR_spi_setup),    MP_ROM_PTR(&pi_spi_setup_obj) },
    { MP_ROM_QSTR(MP_QSTR_spi_transfer), MP_ROM_PTR(&pi_spi_transfer_obj) },
    { MP_ROM_QSTR(MP_QSTR_board),        MP_ROM_PTR(&pi_board_obj) },
    { MP_ROM_QSTR(MP_QSTR_micros),       MP_ROM_PTR(&pi_micros_obj) },
    { MP_ROM_QSTR(MP_QSTR_fb_setup),     MP_ROM_PTR(&pi_fb_setup_obj) },
    { MP_ROM_QSTR(MP_QSTR_fb_rect),      MP_ROM_PTR(&pi_fb_rect_obj) },
    { MP_ROM_QSTR(MP_QSTR_fb_text),      MP_ROM_PTR(&pi_fb_text_obj) },
    { MP_ROM_QSTR(MP_QSTR_INPUT),        MP_ROM_INT(HW_PIN_IN) },
    { MP_ROM_QSTR(MP_QSTR_OUTPUT),       MP_ROM_INT(HW_PIN_OUT) },
    { MP_ROM_QSTR(MP_QSTR_PULL_UP),      MP_ROM_INT(HW_PULL_UP) },
    { MP_ROM_QSTR(MP_QSTR_PULL_DOWN),    MP_ROM_INT(HW_PULL_DOWN) },
    { MP_ROM_QSTR(MP_QSTR_PULL_NONE),    MP_ROM_INT(HW_PULL_NONE) },
};
static MP_DEFINE_CONST_DICT(pi_module_globals, pi_module_globals_table);

const mp_obj_module_t mp_module_pi = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&pi_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_pi, mp_module_pi);
