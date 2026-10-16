#ifndef ARM_CFGFILE_H
#define ARM_CFGFILE_H

#include <stdint.h>

/*
 * config.txt, the Raspberry Pi firmware's settings file on the boot partition.
 *
 * The firmware reads it before the kernel starts (display mode, GPU memory, UART, overlays,
 * clocks...), so editing it is how hardware is configured on a Pi, and `raspi-config` is the
 * tool people use to do it. The edit rules here are raspi-config's own (its set_config_var,
 * clear_config_var and get_config_var), so a file edited here and one edited by raspi-config
 * on Raspberry Pi OS are interchangeable.
 */

#define CFG_MAX 16384

int  cfg_load(void);              /* reads config.txt from the card; 0 = loaded, -1 = no card/file */
int  cfg_loaded(void);            /* 1 once a real file has been read (or created by an edit) */
int  cfg_save(void);              /* writes it back; 0 = ok */
const char *cfg_text(void);
int  cfg_length(void);

/* raspi-config's three primitives. `key` may itself contain '=' ("dtparam=spi"). */
int  cfg_get(const char *key, char *out, int max);      /* 1 if "key=value" is set (uncommented) */
void cfg_set(const char *key, const char *value);       /* replaces every "[#]key=..." line, or appends */
void cfg_clear(const char *key);                        /* comments out "key=..." lines */

/* the sed one-liners raspi-config uses on the file */
void cfg_comment_prefix(const char *prefix);            /* s/^prefix/#prefix/ */
void cfg_uncomment_prefix(const char *prefix);          /* s/^#prefix/prefix/ */
int  cfg_has_prefix(const char *prefix);                /* grep -q "^prefix" */
void cfg_replace_prefix(const char *prefix, const char *line);   /* lines starting with prefix become `line` */
void cfg_delete_containing(const char *text);           /* /text/d */
void cfg_append_line(const char *line);

/* dtparam=a,b=on,c  --  1 if `name` is switched on in any dtparam/device_tree_param line */
int  cfg_dtparam_on(const char *name);
/* 1 if the param appears at all, and its value is off/false/no/0 */
int  cfg_dtparam_off(const char *name);

/* what the kernel itself honours (see bootcfg in cfgfile.c) */
int  bootcfg_interface_allowed(const char *name);       /* "i2c", "spi": raspi-config's switch */
int  bootcfg_framebuffer(uint32_t *w, uint32_t *h);     /* framebuffer_width/height, if set */

#endif
