/* img.c: platform-independent .spr decoder (see img.h). */
#include <stdlib.h>
#include <string.h>

#include "img.h"

static uint32_t RdU32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int Img_LoadSpr(const void *data, size_t len, Image *out) {
    if (!data || !out || len < 20) return 0;
    const uint8_t *b = (const uint8_t *)data;
    if (b[0] != 'H' || b[1] != 'K' || b[2] != 'S' || b[3] != '1') return 0;

    uint32_t ver  = RdU32(b + 4);
    uint32_t w    = RdU32(b + 8);
    uint32_t h    = RdU32(b + 12);
    uint32_t fmt  = RdU32(b + 16);
    if (ver != 1 || fmt != 0 || w == 0 || h == 0) return 0;
    if (w > 4096u || h > 4096u) return 0;
    if (len < 20u + (size_t)w * h * 4u) return 0;

    uint32_t *px = (uint32_t *)malloc((size_t)w * h * 4u);
    if (!px) return 0;

    const uint8_t *s = b + 20;
    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; ++i) {
        uint32_t r = s[i * 4 + 0];
        uint32_t g = s[i * 4 + 1];
        uint32_t bl = s[i * 4 + 2];
        uint32_t a = s[i * 4 + 3];
        px[i] = (a << 24) | (r << 16) | (g << 8) | bl; /* RGBA -> 0xAARRGGBB */
    }

    out->w = w;
    out->h = h;
    out->pixels = px;
    return 1;
}

void Img_Free(Image *img) {
    if (!img) return;
    free(img->pixels);
    img->pixels = 0;
    img->w = img->h = 0;
}

/* ====================================================================== */
/*  PNG decoder (self-contained zlib/inflate; no external dependencies)    */
/* ====================================================================== */

/* --- tiny growable byte buffer used to concatenate IDAT chunks --- */
typedef struct { uint8_t *p; size_t n, cap; int oom; } Buf;
static int Buf_Reserve(Buf *b, size_t extra) {
    if (b->oom) return 0;
    if (b->n + extra <= b->cap) return 1;
    size_t nc = b->cap ? b->cap : 4096;
    while (nc < b->n + extra) nc *= 2;
    uint8_t *np = (uint8_t *)realloc(b->p, nc);
    if (!np) { b->oom = 1; return 0; }
    b->p = np; b->cap = nc;
    return 1;
}
static int Buf_Put(Buf *b, const void *src, size_t n) {
    if (!Buf_Reserve(b, n)) return 0;
    memcpy(b->p + b->n, src, n);
    b->n += n;
    return 1;
}

/* --- inflate (RFC 1951) -------------------------------------------------- */

typedef struct {
    const uint8_t *in;
    size_t         inlen;
    size_t         pos;      /* bit reader: byte index                */
    uint32_t       bitbuf;
    int            bitcnt;
    int            err;
} BitIn;

static uint32_t Bits(BitIn *b, int n) {
    while (b->bitcnt < n) {
        if (b->pos >= b->inlen) { b->err = 1; return 0; }
        b->bitbuf |= (uint32_t)b->in[b->pos++] << b->bitcnt;
        b->bitcnt += 8;
    }
    uint32_t v = b->bitbuf & ((1u << n) - 1u);
    b->bitbuf >>= n;
    b->bitcnt -= n;
    return v;
}
static void AlignByte(BitIn *b) { b->bitbuf = 0; b->bitcnt = 0; }

/* Canonical Huffman decoding table built as (count, symbol) pairs. */
typedef struct {
    uint16_t count[16];
    uint16_t symbol[288];
} Huff;
static void Huff_Build(Huff *h, const uint8_t *lens, int n) {
    int i;
    for (i = 0; i < 16; ++i) h->count[i] = 0;
    for (i = 0; i < n; ++i) h->count[lens[i]]++;
    h->count[0] = 0;
    uint16_t offs[16];
    offs[0] = 0;
    for (i = 1; i < 16; ++i) offs[i] = (uint16_t)(offs[i - 1] + h->count[i - 1]);
    for (i = 0; i < n; ++i)
        if (lens[i]) h->symbol[offs[lens[i]]++] = (uint16_t)i;
}
static int Huff_Decode(BitIn *b, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
        code |= (int)Bits(b, 1);
        if (b->err) return -1;
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first  = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

/* Length / distance base tables (RFC 1951 3.2.5). */
static const uint16_t LEN_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,
    131,163,195,227,258 };
static const uint8_t  LEN_EXTR[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,
    1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t  DIST_EXTR[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

/* Inflates `in` into `out` (a growable buffer). Returns 1 on success. */
static int Inflate(const uint8_t *in, size_t inlen, Buf *out) {
    BitIn b;
    b.in = in; b.inlen = inlen; b.pos = 0; b.bitbuf = 0; b.bitcnt = 0; b.err = 0;

    for (;;) {
        uint32_t last = Bits(&b, 1);
        uint32_t type = Bits(&b, 2);
        if (b.err) return 0;

        if (type == 0) {                       /* stored */
            AlignByte(&b);
            if (b.pos + 4 > inlen) return 0;
            uint32_t len = in[b.pos] | ((uint32_t)in[b.pos + 1] << 8);
            b.pos += 4;                        /* skip LEN + ~LEN */
            if (b.pos + len > inlen) return 0;
            if (!Buf_Put(out, in + b.pos, len)) return 0;
            b.pos += len;
        } else if (type == 1 || type == 2) {   /* fixed or dynamic Huffman */
            Huff lit, dist;
            if (type == 1) {
                uint8_t lens[288];
                int i;
                for (i = 0; i < 144; ++i) lens[i] = 8;
                for (; i < 256; ++i) lens[i] = 9;
                for (; i < 280; ++i) lens[i] = 7;
                for (; i < 288; ++i) lens[i] = 8;
                Huff_Build(&lit, lens, 288);
                for (i = 0; i < 30; ++i) lens[i] = 5;
                Huff_Build(&dist, lens, 30);
            } else {
                uint32_t hlit = Bits(&b, 5) + 257;
                uint32_t hdist = Bits(&b, 5) + 1;
                uint32_t hclen = Bits(&b, 4) + 4;
                if (b.err || hlit > 286 || hdist > 30) return 0;
                static const uint8_t order[19] =
                    {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
                uint8_t clen[19];
                memset(clen, 0, sizeof(clen));
                for (uint32_t i = 0; i < hclen; ++i) clen[order[i]] = (uint8_t)Bits(&b, 3);
                Huff code;
                Huff_Build(&code, clen, 19);

                uint8_t lens[320];
                uint32_t n = 0, total = hlit + hdist;
                while (n < total) {
                    int sym = Huff_Decode(&b, &code);
                    if (sym < 0) return 0;
                    if (sym < 16) {
                        lens[n++] = (uint8_t)sym;
                    } else if (sym == 16) {
                        if (n == 0) return 0;
                        uint8_t prev = lens[n - 1];
                        uint32_t rep = 3 + Bits(&b, 2);
                        while (rep-- && n < total) lens[n++] = prev;
                    } else if (sym == 17) {
                        uint32_t rep = 3 + Bits(&b, 3);
                        while (rep-- && n < total) lens[n++] = 0;
                    } else {
                        uint32_t rep = 11 + Bits(&b, 7);
                        while (rep-- && n < total) lens[n++] = 0;
                    }
                    if (b.err) return 0;
                }
                Huff_Build(&lit, lens, (int)hlit);
                Huff_Build(&dist, lens + hlit, (int)hdist);
            }

            for (;;) {
                int sym = Huff_Decode(&b, &lit);
                if (sym < 0) return 0;
                if (sym < 256) {
                    uint8_t c = (uint8_t)sym;
                    if (!Buf_Put(out, &c, 1)) return 0;
                } else if (sym == 256) {
                    break;
                } else {
                    sym -= 257;
                    if (sym >= 29) return 0;
                    uint32_t length = LEN_BASE[sym] + Bits(&b, LEN_EXTR[sym]);
                    int dsym = Huff_Decode(&b, &dist);
                    if (dsym < 0 || dsym >= 30) return 0;
                    uint32_t d = DIST_BASE[dsym] + Bits(&b, DIST_EXTR[dsym]);
                    if (b.err || d == 0 || d > out->n) return 0;
                    if (!Buf_Reserve(out, length)) return 0;
                    for (uint32_t k = 0; k < length; ++k) {
                        out->p[out->n] = out->p[out->n - d];
                        out->n++;
                    }
                }
                if (b.err) return 0;
            }
        } else {
            return 0;                          /* reserved block type */
        }
        if (last) break;
    }
    return 1;
}

/* zlib wrapper: 2-byte header + deflate + 4-byte Adler-32. */
static int ZlibInflate(const uint8_t *in, size_t inlen, Buf *out) {
    if (inlen < 6) return 0;
    if ((in[0] & 0x0F) != 8) return 0;          /* deflate */
    int cmf = in[0], flg = in[1];
    if (((cmf << 8) + flg) % 31 != 0) return 0;
    if (flg & 0x20) return 0;                    /* preset dictionary: unsupported */
    return Inflate(in + 2, inlen - 2, out);
}

static uint32_t RdU32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static int Paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

int Img_LoadPng(const void *data, size_t len, Image *out) {
    static const uint8_t sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    if (!data || !out || len < 8) return 0;
    const uint8_t *b = (const uint8_t *)data;
    if (memcmp(b, sig, 8) != 0) return 0;

    uint32_t w = 0, h = 0;
    int depth = 0, ctype = 0, interlace = 0;
    int have_ihdr = 0;
    uint8_t palette[256 * 3];
    int palette_n = 0;
    uint8_t trns_pal[256];
    int trns_pal_n = 0;
    uint8_t trns_key[8];
    int trns_key_n = 0;
    Buf idat; memset(&idat, 0, sizeof(idat));

    size_t p = 8;
    while (p + 8 <= len) {
        uint32_t clen = RdU32be(b + p);
        const uint8_t *ctype4 = b + p + 4;
        if ((size_t)clen > len - p - 8) break;    /* truncated */
        const uint8_t *cdata = b + p + 8;

        if (!memcmp(ctype4, "IHDR", 4)) {
            if (clen < 13) { if (idat.p) free(idat.p); return 0; }
            w = RdU32be(cdata);
            h = RdU32be(cdata + 4);
            depth = cdata[8];
            ctype = cdata[9];
            interlace = cdata[12];
            have_ihdr = 1;
        } else if (!memcmp(ctype4, "PLTE", 4)) {
            palette_n = (int)(clen / 3);
            if (palette_n > 256) palette_n = 256;
            memcpy(palette, cdata, (size_t)palette_n * 3);
        } else if (!memcmp(ctype4, "tRNS", 4)) {
            if (ctype == 3) {
                trns_pal_n = (int)clen; if (trns_pal_n > 256) trns_pal_n = 256;
                memcpy(trns_pal, cdata, (size_t)trns_pal_n);
            } else if (ctype == 0 && clen >= 2) {
                trns_key_n = 2; trns_key[0] = cdata[0]; trns_key[1] = cdata[1];
            } else if (ctype == 2 && clen >= 6) {
                trns_key_n = 6; memcpy(trns_key, cdata, 6);
            }
        } else if (!memcmp(ctype4, "IDAT", 4)) {
            if (!Buf_Put(&idat, cdata, clen)) { if (idat.p) free(idat.p); return 0; }
        } else if (!memcmp(ctype4, "IEND", 4)) {
            break;
        }
        p += 12u + clen;
    }

    if (!have_ihdr || w == 0 || h == 0 || w > 4096u || h > 4096u) {
        if (idat.p) free(idat.p);
        return 0;
    }
    if (interlace != 0 || depth != 8) {          /* keep it simple + safe */
        if (idat.p) free(idat.p);
        return 0;
    }
    int channels;
    switch (ctype) {
        case 0: channels = 1; break;             /* grayscale */
        case 2: channels = 3; break;             /* RGB       */
        case 3: channels = 1; break;             /* palette   */
        case 4: channels = 2; break;             /* gray+alpha*/
        case 6: channels = 4; break;             /* RGBA      */
        default: if (idat.p) free(idat.p); return 0;
    }

    Buf raw; memset(&raw, 0, sizeof(raw));
    if (!ZlibInflate(idat.p, idat.n, &raw)) { free(idat.p); if (raw.p) free(raw.p); return 0; }
    free(idat.p);

    size_t stride = (size_t)w * channels;
    if (raw.n < (stride + 1) * h) { free(raw.p); return 0; }

    uint8_t *img = (uint8_t *)malloc(stride * h);
    uint32_t *px = (uint32_t *)malloc((size_t)w * h * 4u);
    if (!img || !px) { free(img); free(px); free(raw.p); return 0; }

    /* Undo the per-scanline filters. */
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t *src = raw.p + y * (stride + 1);
        int ft = src[0];
        const uint8_t *in = src + 1;
        uint8_t *cur = img + y * stride;
        uint8_t *up  = y ? img + (y - 1) * stride : 0;
        for (size_t x = 0; x < stride; ++x) {
            int a = x >= (size_t)channels ? cur[x - channels] : 0;
            int bb = up ? up[x] : 0;
            int c = (up && x >= (size_t)channels) ? up[x - channels] : 0;
            int v = in[x];
            switch (ft) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += bb; break;
                case 3: v += (a + bb) >> 1; break;
                case 4: v += Paeth(a, bb, c); break;
                default: free(img); free(px); free(raw.p); return 0;
            }
            cur[x] = (uint8_t)v;
        }
    }
    free(raw.p);

    /* Convert to 0xAARRGGBB. */
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t *row = img + y * stride;
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t r, g, bl, al = 255;
            const uint8_t *s = row + (size_t)x * channels;
            if (ctype == 0) {
                r = g = bl = s[0];
                if (trns_key_n == 2 && s[0] == trns_key[0]) al = 0;
            } else if (ctype == 2) {
                r = s[0]; g = s[1]; bl = s[2];
                if (trns_key_n == 6 && s[0] == trns_key[0] && s[1] == trns_key[1] &&
                    s[2] == trns_key[2]) al = 0;
            } else if (ctype == 3) {
                int idx = s[0];
                if (idx >= palette_n) { r = g = bl = 0; }
                else { r = palette[idx*3+0]; g = palette[idx*3+1]; bl = palette[idx*3+2]; }
                if (idx < trns_pal_n) al = trns_pal[idx];
            } else if (ctype == 4) {
                r = g = bl = s[0]; al = s[1];
            } else { /* 6 */
                r = s[0]; g = s[1]; bl = s[2]; al = s[3];
            }
            px[(size_t)y * w + x] = (al << 24) | (r << 16) | (g << 8) | bl;
        }
    }
    free(img);

    out->w = w;
    out->h = h;
    out->pixels = px;
    return 1;
}

/* --- BMP ----------------------------------------------------------------- */

static uint16_t RdU16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t RdU32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int Img_LoadBmp(const void *data, size_t len, Image *out) {
    if (!data || !out || len < 54) return 0;
    const uint8_t *b = (const uint8_t *)data;
    if (b[0] != 'B' || b[1] != 'M') return 0;

    uint32_t data_off = RdU32le(b + 10);
    /* BITMAPINFOHEADER */
    uint32_t hdr_sz = RdU32le(b + 14);
    if (hdr_sz < 40) return 0;
    int32_t  w  = (int32_t)RdU32le(b + 18);
    int32_t  hh = (int32_t)RdU32le(b + 22);
    uint16_t bpp = RdU16le(b + 28);
    uint32_t comp = RdU32le(b + 30);
    if (comp != 0) return 0;                     /* BI_RGB only */
    if (w <= 0 || w > 4096 || hh == 0) return 0;
    int top_down = hh < 0;
    uint32_t h = (uint32_t)(hh < 0 ? -hh : hh);
    if (h > 4096) return 0;
    if (bpp != 24 && bpp != 32) return 0;

    uint32_t rowbytes = ((uint32_t)w * bpp / 8u + 3u) & ~3u;
    if ((size_t)data_off + (size_t)rowbytes * h > len) return 0;

    uint32_t *px = (uint32_t *)malloc((size_t)w * h * 4u);
    if (!px) return 0;

    for (uint32_t y = 0; y < h; ++y) {
        uint32_t sy = top_down ? y : (h - 1 - y);
        const uint8_t *row = b + data_off + (size_t)sy * rowbytes;
        for (uint32_t x = 0; x < (uint32_t)w; ++x) {
            uint32_t bl = row[x * (bpp / 8) + 0];
            uint32_t g  = row[x * (bpp / 8) + 1];
            uint32_t r  = row[x * (bpp / 8) + 2];
            uint32_t a  = (bpp == 32) ? row[x * 4 + 3] : 255;
            if (bpp == 32 && a == 0) a = 255;    /* many BMPs leave A=0 */
            px[(size_t)y * w + x] = (a << 24) | (r << 16) | (g << 8) | bl;
        }
    }
    out->w = w;
    out->h = h;
    out->pixels = px;
    return 1;
}

int Img_LoadAuto(const void *data, size_t len, Image *out) {
    if (Img_LoadPng(data, len, out)) return 1;
    if (Img_LoadBmp(data, len, out)) return 1;
    return Img_LoadSpr(data, len, out);
}

int Img_IsSupported(const void *data, size_t len) {
    static const uint8_t sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
    if (!data || len < 2) return 0;
    const uint8_t *b = (const uint8_t *)data;
    if (len >= 8 && memcmp(b, sig, 8) == 0) return 1;
    if (b[0] == 'B' && b[1] == 'M') return 1;
    if (len >= 4 && b[0] == 'H' && b[1] == 'K' && b[2] == 'S' && b[3] == '1') return 1;
    return 0;
}

int Img_ScaleToFit(const Image *src, uint32_t maxW, uint32_t maxH, Image *out) {
    if (!src || !src->pixels || !out || src->w == 0 || src->h == 0) return 0;
    if (maxW == 0 || maxH == 0) return 0;
    uint32_t dw = src->w, dh = src->h;
    if (dw > maxW || dh > maxH) {
        /* Pick the smaller scale factor (integer-free, nearest neighbour). */
        uint32_t sw = (uint32_t)((uint64_t)src->w * maxH / src->h);
        uint32_t sh = (uint32_t)((uint64_t)src->h * maxW / src->w);
        if (sw <= maxW) { dw = sw; dh = maxH; }
        else            { dw = maxW; dh = sh; }
        if (dw == 0) dw = 1;
        if (dh == 0) dh = 1;
    }
    if (dw == src->w && dh == src->h) {          /* already small enough */
        uint32_t *cp = (uint32_t *)malloc((size_t)dw * dh * 4u);
        if (!cp) return 0;
        memcpy(cp, src->pixels, (size_t)dw * dh * 4u);
        out->w = dw; out->h = dh; out->pixels = cp;
        return 1;
    }
    uint32_t *px = (uint32_t *)malloc((size_t)dw * dh * 4u);
    if (!px) return 0;
    for (uint32_t y = 0; y < dh; ++y) {
        uint32_t sy = (uint32_t)((uint64_t)y * src->h / dh);
        for (uint32_t x = 0; x < dw; ++x) {
            uint32_t sx = (uint32_t)((uint64_t)x * src->w / dw);
            px[(size_t)y * dw + x] = src->pixels[(size_t)sy * src->w + sx];
        }
    }
    out->w = dw; out->h = dh; out->pixels = px;
    return 1;
}
