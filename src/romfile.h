#ifndef ROMFILE_H
#define ROMFILE_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
/* Loads a .nes file, or the first .nes entry inside a .zip. Caller frees. */
uint8_t *romfile_load(const char *path, size_t *len, char *inner_name, size_t name_cap, char *err, size_t errlen);
#endif
