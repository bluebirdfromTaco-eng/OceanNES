/*
 * ROM file loading: plain .nes files and .zip archives (first .nes entry).
 */
#include "romfile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); }

static bool ends_with_nes(const char *name, size_t len) {
    return len >= 4 && strncasecmp(name + len - 4, ".nes", 4) == 0;
}

static uint8_t *zip_extract(const uint8_t *z, size_t zlen, size_t *out_len, char *name_out, size_t name_cap) {
    /* walk the central directory so sizes are reliable even with data descriptors */
    if (zlen < 22) return NULL;
    size_t eocd = 0; bool found = false;
    for (size_t i = zlen - 22 + 1; i-- > 0 && zlen - i < 65557;) {
        if (rd32(z + i) == 0x06054b50) { eocd = i; found = true; break; }
    }
    if (!found) return NULL;
    uint32_t cd_off = rd32(z + eocd + 16);
    uint16_t entries = rd16(z + eocd + 10);
    size_t p = cd_off;
    for (int e = 0; e < entries; e++) {
        if (p + 46 > zlen || rd32(z + p) != 0x02014b50) return NULL;
        uint16_t method = rd16(z + p + 10);
        uint32_t csize = rd32(z + p + 20), usize = rd32(z + p + 24);
        uint16_t nlen = rd16(z + p + 28), xlen = rd16(z + p + 30), clen = rd16(z + p + 32);
        uint32_t loff = rd32(z + p + 42);
        const char *name = (const char *)z + p + 46;
        if (p + 46 + nlen > zlen) return NULL;
        if (ends_with_nes(name, nlen) && usize > 16 && usize < 64u * 1024 * 1024) {
            if (loff + 30 > zlen || rd32(z + loff) != 0x04034b50) return NULL;
            size_t data = loff + 30 + rd16(z + loff + 26) + rd16(z + loff + 28);
            if (data + csize > zlen) return NULL;
            uint8_t *out = malloc(usize);
            if (!out) return NULL;
            if (method == 0) {
                if (csize != usize) { free(out); return NULL; }
                memcpy(out, z + data, usize);
            } else if (method == 8) {
                z_stream s; memset(&s, 0, sizeof s);
                if (inflateInit2(&s, -15) != Z_OK) { free(out); return NULL; }
                s.next_in = (Bytef *)(z + data); s.avail_in = csize;
                s.next_out = out; s.avail_out = usize;
                int r = inflate(&s, Z_FINISH);
                inflateEnd(&s);
                if (r != Z_STREAM_END) { free(out); return NULL; }
            } else { free(out); return NULL; }
            if (name_out && name_cap) {
                size_t k = nlen < name_cap - 1 ? nlen : name_cap - 1;
                memcpy(name_out, name, k); name_out[k] = 0;
            }
            *out_len = usize;
            return out;
        }
        p += 46 + nlen + xlen + clen;
    }
    return NULL;
}

uint8_t *romfile_load(const char *path, size_t *len, char *inner_name, size_t name_cap, char *err, size_t errlen) {
    if (inner_name && name_cap) inner_name[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errlen, "Couldn't open the file."); return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 128L * 1024 * 1024) { fclose(f); snprintf(err, errlen, "The file is empty or too large."); return NULL; }
    uint8_t *d = malloc((size_t)sz);
    if (!d || fread(d, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(d); snprintf(err, errlen, "Couldn't read the file."); return NULL; }
    fclose(f);
    if (sz >= 4 && rd32(d) == 0x04034b50) {
        size_t ulen = 0;
        uint8_t *u = zip_extract(d, (size_t)sz, &ulen, inner_name, name_cap);
        free(d);
        if (!u) { snprintf(err, errlen, "No .nes file found in this zip archive."); return NULL; }
        *len = ulen;
        return u;
    }
    *len = (size_t)sz;
    return d;
}
