#include "png.h"
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PNG_MAX_BYTES (2 * 1024 * 1024)
#define HUFF_BITS 15

struct huff {
    int count[HUFF_BITS + 1];
    int symbol[320];
};

struct bits {
    const unsigned char *p;
    const unsigned char *end;
    unsigned acc;
    int n;
};

static unsigned long crc_table[256];
static int crc_ready;

static void crc_init(void) {
    if (crc_ready) return;
    for (unsigned n = 0; n < 256; n++) {
        unsigned c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
    crc_ready = 1;
}

static unsigned long chunk_crc(const unsigned char *type, const unsigned char *data, unsigned long n) {
    unsigned long c = 0xffffffffu;
    crc_init();
    for (int i = 0; i < 4; i++) c = crc_table[(c ^ type[i]) & 0xff] ^ (c >> 8);
    for (unsigned long i = 0; i < n; i++) c = crc_table[(c ^ data[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

static int bit_get(struct bits *b, int n) {
    if (n == 0) return 0;
    while (b->n < n) {
        if (b->p >= b->end) return -1;
        b->acc |= (unsigned)*b->p++ << b->n;
        b->n += 8;
    }
    int v = (int)(b->acc & ((1u << n) - 1));
    b->acc >>= n;
    b->n -= n;
    return v;
}

static void bit_align(struct bits *b) {
    int drop = b->n & 7;
    b->acc >>= drop;
    b->n -= drop;
}

static int huff_build(struct huff *h, const int *len, int n) {
    int offs[HUFF_BITS + 1];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) {
        if (len[i] < 0 || len[i] > HUFF_BITS) return -1;
        h->count[len[i]]++;
    }
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int bits = 1; bits <= HUFF_BITS; bits++) {
        left <<= 1;
        left -= h->count[bits];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (int bits = 1; bits < HUFF_BITS; bits++) offs[bits + 1] = offs[bits] + h->count[bits];
    for (int i = 0; i < n; i++) {
        if (len[i] != 0) h->symbol[offs[len[i]]++] = i;
    }
    return 0;
}

static int huff_decode(struct huff *h, struct bits *b) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= HUFF_BITS; len++) {
        int bit = bit_get(b, 1);
        if (bit < 0) return -1;
        code |= bit;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const int len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const int len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const int dist_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                  193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const int dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                   6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const int cl_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

static int inflate_codes(struct bits *b, struct huff *lit, struct huff *dist, unsigned char *out, size_t cap, size_t *used) {
    for (;;) {
        int sym = huff_decode(lit, b);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (*used >= cap) return -1;
            out[(*used)++] = (unsigned char)sym;
            continue;
        }
        if (sym == 256) return 0;
        if (sym > 285) return -1;
        int extra = bit_get(b, len_extra[sym - 257]);
        if (extra < 0) return -1;
        int length = len_base[sym - 257] + extra;
        int dsym = huff_decode(dist, b);
        if (dsym < 0 || dsym > 29) return -1;
        extra = bit_get(b, dist_extra[dsym]);
        if (extra < 0) return -1;
        int distance = dist_base[dsym] + extra;
        if (distance <= 0 || (size_t)distance > *used) return -1;
        if (*used + (size_t)length > cap) return -1;
        for (int i = 0; i < length; i++) {
            out[*used] = out[*used - (size_t)distance];
            (*used)++;
        }
    }
}

static int inflate_zlib(const unsigned char *src, size_t n, unsigned char *out, size_t cap) {
    if (n < 6) return -1;
    unsigned cmf = src[0], flg = src[1];
    if ((cmf * 256u + flg) % 31u != 0) return -1;
    if ((cmf & 0x0f) != 8 || (flg & 0x20)) return -1;
    struct bits b = {.p = src + 2, .end = src + n - 4, .acc = 0, .n = 0};
    size_t used = 0;
    int bfinal;
    do {
        bfinal = bit_get(&b, 1);
        int btype = bit_get(&b, 2);
        if (bfinal < 0 || btype < 0) return -1;
        if (btype == 0) {
            bit_align(&b);
            int len = bit_get(&b, 16);
            int nlen = bit_get(&b, 16);
            if (len < 0 || nlen < 0 || (len ^ 0xffff) != nlen) return -1;
            if (used + (size_t)len > cap) return -1;
            for (int i = 0; i < len; i++) {
                int byte = bit_get(&b, 8);
                if (byte < 0) return -1;
                out[used++] = (unsigned char)byte;
            }
        } else if (btype == 1 || btype == 2) {
            struct huff lit, dist;
            if (btype == 1) {
                int lens[288];
                for (int i = 0; i < 144; i++) lens[i] = 8;
                for (int i = 144; i < 256; i++) lens[i] = 9;
                for (int i = 256; i < 280; i++) lens[i] = 7;
                for (int i = 280; i < 288; i++) lens[i] = 8;
                int dlens[32];
                for (int i = 0; i < 32; i++) dlens[i] = 5;
                if (huff_build(&lit, lens, 288) != 0 || huff_build(&dist, dlens, 32) != 0) return -1;
            } else {
                int hlit = bit_get(&b, 5);
                int hdist = bit_get(&b, 5);
                int hclen = bit_get(&b, 4);
                if (hlit < 0 || hdist < 0 || hclen < 0) return -1;
                hlit += 257;
                hdist += 1;
                hclen += 4;
                if (hlit > 286 || hdist > 32) return -1;
                int clen[19];
                memset(clen, 0, sizeof clen);
                for (int i = 0; i < hclen; i++) {
                    int v = bit_get(&b, 3);
                    if (v < 0) return -1;
                    clen[cl_order[i]] = v;
                }
                struct huff cl;
                if (huff_build(&cl, clen, 19) != 0) return -1;
                int lens[320];
                memset(lens, 0, sizeof lens);
                int total = hlit + hdist;
                for (int i = 0; i < total;) {
                    int sym = huff_decode(&cl, &b);
                    if (sym < 0) return -1;
                    int repeat = 1, value = sym;
                    if (sym == 16) {
                        if (i == 0) return -1;
                        int extra = bit_get(&b, 2);
                        if (extra < 0) return -1;
                        repeat = 3 + extra;
                        value = lens[i - 1];
                    } else if (sym == 17) {
                        int extra = bit_get(&b, 3);
                        if (extra < 0) return -1;
                        repeat = 3 + extra;
                        value = 0;
                    } else if (sym == 18) {
                        int extra = bit_get(&b, 7);
                        if (extra < 0) return -1;
                        repeat = 11 + extra;
                        value = 0;
                    } else if (sym > 15) {
                        return -1;
                    }
                    if (i + repeat > total) return -1;
                    for (int k = 0; k < repeat; k++) lens[i++] = value;
                }
                if (huff_build(&lit, lens, hlit) != 0 || huff_build(&dist, lens + hlit, hdist) != 0) return -1;
            }
            if (inflate_codes(&b, &lit, &dist, out, cap, &used) != 0) return -1;
        } else {
            return -1;
        }
    } while (!bfinal);
    if (used != cap) return -1;
    unsigned long a = 1, s = 0;
    for (size_t i = 0; i < used; i++) {
        a = (a + out[i]) % 65521u;
        s = (s + a) % 65521u;
    }
    unsigned long got = ((unsigned long)src[n - 4] << 24) | ((unsigned long)src[n - 3] << 16) |
                        ((unsigned long)src[n - 2] << 8) | src[n - 1];
    if (((s << 16) | a) != got) return -1;
    return 0;
}

static int paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static uint32_t over_black(int r, int g, int b, int a) {
    r = r * a / 255;
    g = g * a / 255;
    b = b * a / 255;
    return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

int png_load_screen(const char *path, uint32_t *px) {
    FILE *f;
    unsigned char *file = NULL, *idat = NULL, *raw = NULL, *prev = NULL, *cur = NULL;
    unsigned char *pal = NULL, *trns = NULL;
    long pal_n = 0, trns_n = 0, idat_n = 0, idat_cap = 0;
    int rc = -1;
    int width = 0, height = 0, bit = 0, color = 0, inter = 0;
    int have_ihdr = 0;
    if (!path || !px) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) goto done;
    long sz = ftell(f);
    if (sz < 8 || sz > PNG_MAX_BYTES) goto done;
    if (fseek(f, 0, SEEK_SET) != 0) goto done;
    file = malloc((size_t)sz);
    if (!file || fread(file, 1, (size_t)sz, f) != (size_t)sz) goto done;
    static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (memcmp(file, sig, 8) != 0) goto done;
    size_t i = 8;
    while (i + 12 <= (size_t)sz) {
        unsigned long len = ((unsigned long)file[i] << 24) | ((unsigned long)file[i + 1] << 16) |
                            ((unsigned long)file[i + 2] << 8) | file[i + 3];
        unsigned char *type = file + i + 4;
        if (i + 12 + len > (size_t)sz) goto done;
        unsigned char *data = file + i + 8;
        unsigned long crc = ((unsigned long)data[len] << 24) | ((unsigned long)data[len + 1] << 16) |
                            ((unsigned long)data[len + 2] << 8) | data[len + 3];
        if (chunk_crc(type, data, len) != crc) goto done;
        int ancillary = (type[0] & 0x20) != 0;
        if (memcmp(type, "IHDR", 4) == 0) {
            if (len != 13 || have_ihdr) goto done;
            unsigned uw = ((unsigned)data[0] << 24) | ((unsigned)data[1] << 16) | ((unsigned)data[2] << 8) | data[3];
            unsigned uh = ((unsigned)data[4] << 24) | ((unsigned)data[5] << 16) | ((unsigned)data[6] << 8) | data[7];
            if (uw != (unsigned)UI_W || uh != (unsigned)UI_H) goto done;
            width = UI_W;
            height = UI_H;
            bit = data[8];
            color = data[9];
            inter = data[12];
            if (data[10] != 0 || data[11] != 0) goto done;
            have_ihdr = 1;
        } else if (memcmp(type, "PLTE", 4) == 0) {
            if (!have_ihdr || len == 0 || len > 256 * 3 || len % 3 != 0) goto done;
            free(pal);
            pal = malloc(len);
            if (!pal) goto done;
            memcpy(pal, data, len);
            pal_n = (long)len;
        } else if (memcmp(type, "tRNS", 4) == 0) {
            if (!have_ihdr || len > 256) goto done;
            free(trns);
            trns = malloc(len ? len : 1);
            if (!trns) goto done;
            if (len) memcpy(trns, data, len);
            trns_n = (long)len;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (!have_ihdr) goto done;
            if (idat_n + (long)len > PNG_MAX_BYTES) goto done;
            if (idat_n + (long)len > idat_cap) {
                long cap = idat_cap ? idat_cap * 2 : 8192;
                while (cap < idat_n + (long)len) cap *= 2;
                unsigned char *grown = realloc(idat, (size_t)cap);
                if (!grown) goto done;
                idat = grown;
                idat_cap = cap;
            }
            memcpy(idat + idat_n, data, len);
            idat_n += (long)len;
        } else if (memcmp(type, "IEND", 4) == 0) {
            break;
        } else if (!ancillary) {
            goto done;
        }
        i += 12 + len;
    }
    if (!have_ihdr || width != UI_W || height != UI_H || inter != 0 || idat_n < 6) goto done;
    int channels = 0;
    if (color == 0) channels = 1;
    else if (color == 2) channels = 3;
    else if (color == 3) channels = 1;
    else if (color == 4) channels = 2;
    else if (color == 6) channels = 4;
    else goto done;
    if (color == 3) {
        if (bit != 1 && bit != 2 && bit != 4 && bit != 8) goto done;
        if (!pal || pal_n < 3) goto done;
    } else if (bit != 8) {
        goto done;
    }
    int samples = width;
    int row_bytes;
    if (bit == 8) row_bytes = width * channels;
    else row_bytes = (width * bit + 7) / 8;
    size_t raw_n = (size_t)height * (size_t)(row_bytes + 1);
    raw = malloc(raw_n);
    prev = calloc((size_t)row_bytes, 1);
    cur = malloc((size_t)row_bytes);
    if (!raw || !prev || !cur || inflate_zlib(idat, (size_t)idat_n, raw, raw_n) != 0) goto done;
    int bpp = bit == 8 ? channels : 1;
    const unsigned char *src = raw;
    for (int y = 0; y < height; y++) {
        int filter = *src++;
        if (filter > 4) goto done;
        for (int x = 0; x < row_bytes; x++) {
            int left = x >= bpp ? cur[x - bpp] : 0;
            int up = prev[x];
            int ul = x >= bpp ? prev[x - bpp] : 0;
            int v = *src++;
            if (filter == 1) v += left;
            else if (filter == 2) v += up;
            else if (filter == 3) v += (left + up) / 2;
            else if (filter == 4) v += paeth(left, up, ul);
            cur[x] = (unsigned char)v;
        }
        for (int x = 0; x < samples; x++) {
            int r = 0, g = 0, b = 0, a = 255;
            if (color == 3) {
                int idx;
                if (bit == 8) idx = cur[x];
                else {
                    int pack = 8 / bit;
                    int shift = (pack - 1 - (x % pack)) * bit;
                    idx = (cur[x / pack] >> shift) & ((1 << bit) - 1);
                }
                if (idx < 0 || idx >= pal_n / 3) goto done;
                r = pal[idx * 3];
                g = pal[idx * 3 + 1];
                b = pal[idx * 3 + 2];
                if (trns && idx < trns_n) a = trns[idx];
            } else if (color == 0) {
                r = g = b = cur[x];
                if (trns && trns_n >= 2 && trns[0] == 0 && cur[x] == trns[1]) a = 0;
            } else if (color == 4) {
                r = g = b = cur[x * 2];
                a = cur[x * 2 + 1];
            } else if (color == 2) {
                r = cur[x * 3];
                g = cur[x * 3 + 1];
                b = cur[x * 3 + 2];
                if (trns && trns_n >= 6 && trns[0] == 0 && trns[2] == 0 && trns[4] == 0 && r == trns[1] && g == trns[3] &&
                    b == trns[5])
                    a = 0;
            } else {
                r = cur[x * 4];
                g = cur[x * 4 + 1];
                b = cur[x * 4 + 2];
                a = cur[x * 4 + 3];
            }
            px[y * UI_W + x] = over_black(r, g, b, a);
        }
        memcpy(prev, cur, (size_t)row_bytes);
    }
    rc = 0;
done:
    if (f) fclose(f);
    free(file);
    free(idat);
    free(raw);
    free(prev);
    free(cur);
    free(pal);
    free(trns);
    return rc;
}
