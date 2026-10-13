#include "appicon.h"

#include <stdlib.h>
#include <string.h>

#include "res.h"

bool appicon_from_ne(const ne_image *ne, int want, appicon *out) {
    size_t t;
    memset(out, 0, sizeof *out);
    for (t = 0; t < ne->restype_count; t++) {
        const ne_restype *ty = &ne->restypes[t];
        res_group_pick pk;
        const uint8_t *icon;
        size_t ilen;
        res_image im;
        int i;
        if (ty->name[0] || ty->id != RT_GROUP_ICON || !ty->count) continue;
        if (ty->res[0].offset + (uint64_t)ty->res[0].length > ne->size) return false;
        if (res_group_pick_best(ne->data + ty->res[0].offset, ty->res[0].length, false, want, want, &pk) != RES_OK) return false;
        if (res_find(ne, RT_ICON, false, pk.id, NULL, &icon, &ilen) != RES_OK) return false;
        if (res_icon_decode(icon, ilen, &im) != RES_OK) return false;
        out->argb = malloc((size_t)im.w * (size_t)im.h * sizeof *out->argb);
        if (!out->argb) {
            res_image_free(&im);
            return false;
        }
        out->w = im.w;
        out->h = im.h;
        for (i = 0; i < im.w * im.h; i++) {
            uint32_t c = im.cref[i];                       /* COLORREF: 0x00BBGGRR */
            out->argb[i] = im.mask && im.mask[i] ? 0u : 0xFF000000u | (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF);
        }
        res_image_free(&im);
        return true;
    }
    return false;
}

void appicon_free(appicon *ic) {
    free(ic->argb);
    memset(ic, 0, sizeof *ic);
}

bool appicon_scale(const appicon *src, int factor, appicon *out) {
    int x, y, w, h;
    memset(out, 0, sizeof *out);
    if (!src || !src->argb || src->w <= 0 || src->h <= 0 || factor < 1 || factor > 16) return false;
    w = src->w * factor;
    h = src->h * factor;
    out->argb = malloc((size_t)w * (size_t)h * sizeof *out->argb);
    if (!out->argb) return false;
    out->w = w;
    out->h = h;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) out->argb[y * w + x] = src->argb[(y / factor) * src->w + x / factor];
    return true;
}

static uint32_t crc32_update(uint32_t c, const uint8_t *p, size_t n) {
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(c & 1));
    }
    return c;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* writes one chunk (length, type, data, CRC) at p; returns its size */
static size_t chunk(uint8_t *p, const char *type, const uint8_t *data, size_t n) {
    put32(p, (uint32_t)n);
    memcpy(p + 4, type, 4);
    if (n) memcpy(p + 8, data, n);
    put32(p + 8 + n, ~crc32_update(0xFFFFFFFFu, p + 4, 4 + n));
    return 12 + n;
}

bool appicon_png(const appicon *ic, uint8_t **png, size_t *len) {
    size_t rawn, nblocks, zn, pos = 0, k, i;
    uint8_t *raw, *z, *buf, ihdr[13];
    uint32_t a = 1, b = 0;
    int x, y;
    *png = NULL;
    *len = 0;
    if (!ic || !ic->argb || ic->w <= 0 || ic->h <= 0 || ic->w > 1024 || ic->h > 1024) return false;
    rawn = (size_t)ic->h * (1 + (size_t)ic->w * 4);
    nblocks = (rawn + 65534) / 65535;
    zn = 2 + rawn + nblocks * 5 + 4;
    raw = malloc(rawn);
    z = malloc(zn);
    buf = malloc(8 + 25 + 12 + zn + 12);
    if (!raw || !z || !buf) {
        free(raw);
        free(z);
        free(buf);
        return false;
    }
    for (y = 0; y < ic->h; y++) {
        uint8_t *r = raw + (size_t)y * (1 + (size_t)ic->w * 4);
        *r++ = 0;
        for (x = 0; x < ic->w; x++) {
            uint32_t v = ic->argb[(size_t)y * (size_t)ic->w + (size_t)x];
            *r++ = (uint8_t)(v >> 16);
            *r++ = (uint8_t)(v >> 8);
            *r++ = (uint8_t)v;
            *r++ = (uint8_t)(v >> 24);
        }
    }
    for (i = 0; i < rawn; i++) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    z[pos++] = 0x78;
    z[pos++] = 0x01;
    for (k = 0; k < rawn;) {
        size_t n = rawn - k > 65535 ? 65535 : rawn - k;
        z[pos++] = (uint8_t)(k + n == rawn);
        z[pos++] = (uint8_t)n;
        z[pos++] = (uint8_t)(n >> 8);
        z[pos++] = (uint8_t)~n;
        z[pos++] = (uint8_t)(~n >> 8);
        memcpy(z + pos, raw + k, n);
        pos += n;
        k += n;
    }
    put32(z + pos, b << 16 | a);
    pos += 4;
    put32(ihdr, (uint32_t)ic->w);
    put32(ihdr + 4, (uint32_t)ic->h);
    ihdr[8] = 8;
    ihdr[9] = 6;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    memcpy(buf, "\x89PNG\r\n\x1a\n", 8);
    k = 8;
    k += chunk(buf + k, "IHDR", ihdr, 13);
    k += chunk(buf + k, "IDAT", z, pos);
    k += chunk(buf + k, "IEND", NULL, 0);
    free(raw);
    free(z);
    *png = buf;
    *len = k;
    return true;
}
