/*
 * comp16.h - pixel maths of the game's terrain compositor (0x541250, 16-bit
 * path), shared by ZaxHiRes and comp16_test.c. No Windows dependencies.
 */
#ifndef COMP16_H
#define COMP16_H

#include <immintrin.h>

typedef unsigned char  c16_u8;
typedef unsigned short c16_u16;
typedef unsigned int   c16_u32;

/* Exact replica of the game's integer blend (all arithmetic mod 2^32). */
static c16_u16 blend565(c16_u32 c, c16_u32 d, c16_u32 w)
{
    c16_u32 w4 = w >> 2, w8 = w >> 3;
    c16_u32 g = ((((c >> 5) & 63) - ((d >> 5) & 63)) * w4 >> 1) + (d & 0xffe0);
    c16_u32 r = (((c >> 11) - (d >> 11)) * w8 << 6) + (d & 0xf83f);
    c16_u32 b = ((c & 31) - (d & 31)) * w8 >> 5;
    return (c16_u16)(b + (g & 0x7e0) + (r & 0xf800) + (d & 0x1f));
}

/*
 * Per-call preparation for row_avx2 (see there): texture pair id per layer
 * index, the pair's base/overlay textures, and the weight split as the blend
 * uses it. ok = 0 if some weight is too big for 16-bit lanes.
 */
typedef struct {
    c16_u8  pid[256];
    c16_u32 pair_base[256], pair_over[256];
    c16_u16 w4[256], w8[256];
    int     ok;
} C16Prep;

typedef struct {
    const c16_u32 *base, *over, *weight;  /* per layer index */
    const C16Prep *prep;                  /* for row_avx2; NULL = scalar */
} Layers;

static void c16_prepare(const c16_u32 *base, const c16_u32 *over, const c16_u32 *weight, C16Prep *p)
{
    c16_u32 i, k, n = 0;

    p->ok = 1;
    /* indices with an overlay: one id per distinct (base, overlay) */
    for (i = 0; i < 256; i++) {
        if (!over[i])
            continue;
        for (k = 0; k < n && !(p->pair_base[k] == base[i] && p->pair_over[k] == over[i]); k++)
            ;
        if (k == n) {
            p->pair_base[n] = base[i];
            p->pair_over[n++] = over[i];
        }
        p->pid[i] = (c16_u8)k;
        p->w4[i] = (c16_u16)(weight[i] >> 2);
        p->w8[i] = (c16_u16)(weight[i] >> 3);
        if (weight[i] > 2047)
            p->ok = 0;
    }
    /* base only: weight 0 yields the base pixel whatever the overlay, so join
       any pair on the same base */
    for (i = 0; i < 256; i++) {
        if (over[i])
            continue;
        for (k = 0; k < n && p->pair_base[k] != base[i]; k++)
            ;
        if (k == n) {
            p->pair_base[n] = base[i];
            p->pair_over[n++] = 0;
        }
        p->pid[i] = (c16_u8)k;
        p->w4[i] = p->w8[i] = 0;
    }
}

static void row_c(c16_u16 *dst, const c16_u8 *mask, c16_u32 n, c16_u32 off, const Layers *l)
{
    c16_u32 x;
    for (x = 0; x < n; x++) {
        c16_u32 idx = mask[x], o = off + x * 2;
        c16_u16 d = *(const c16_u16 *)(l->base[idx] + o);
        c16_u32 ov = l->over[idx];
        dst[x] = ov ? blend565(*(const c16_u16 *)(ov + o), d, l->weight[idx]) : d;
    }
}

/*
 * AVX2, 16 pixels per step. The layer index encodes a texture pair plus a
 * blend weight (bands of 26 indices: base A / overlay B with weights 9..249,
 * then B / C, ...), so neighbouring pixels usually share their textures but
 * not their index. Per group of 16: look up each lane's pair id and weight
 * (C16Prep), then blend once per distinct pair in the group and merge lanes.
 *
 * In 16-bit lanes everything the result depends on survives mod 2^16, except
 * blue's logical >> 5 of a possibly negative 32-bit product: its low 16 bits
 * equal an arithmetic >> 5 of the 16-bit product while |product| < 2^15,
 * i.e. weight <= 2047 (C16Prep.ok).
 */
__attribute__((target("avx2")))
static __m256i blend565_x16(__m256i c, __m256i d, __m256i w4, __m256i w8)
{
    const __m256i m31 = _mm256_set1_epi16(31), m63 = _mm256_set1_epi16(63);
    const __m256i mgd = _mm256_set1_epi16((short)0xffe0), mg = _mm256_set1_epi16(0x7e0);
    const __m256i mrd = _mm256_set1_epi16((short)0xf83f), mr = _mm256_set1_epi16((short)0xf800);
    __m256i g, r, b;

    g = _mm256_sub_epi16(_mm256_and_si256(_mm256_srli_epi16(c, 5), m63),
                         _mm256_and_si256(_mm256_srli_epi16(d, 5), m63));
    g = _mm256_add_epi16(_mm256_srli_epi16(_mm256_mullo_epi16(g, w4), 1), _mm256_and_si256(d, mgd));
    r = _mm256_sub_epi16(_mm256_srli_epi16(c, 11), _mm256_srli_epi16(d, 11));
    r = _mm256_add_epi16(_mm256_slli_epi16(_mm256_mullo_epi16(r, w8), 6), _mm256_and_si256(d, mrd));
    b = _mm256_sub_epi16(_mm256_and_si256(c, m31), _mm256_and_si256(d, m31));
    b = _mm256_srai_epi16(_mm256_mullo_epi16(b, w8), 5);
    return _mm256_add_epi16(_mm256_add_epi16(b, _mm256_and_si256(d, m31)),
                            _mm256_add_epi16(_mm256_and_si256(g, mg), _mm256_and_si256(r, mr)));
}

__attribute__((target("avx2")))
static void row_avx2(c16_u16 *dst, const c16_u8 *mask, c16_u32 n, c16_u32 off, const Layers *l)
{
    const C16Prep *p = l->prep;
    c16_u32 x = 0;

    if (!p || !p->ok) {
        row_c(dst, mask, n, off, l);
        return;
    }
    while (x + 16 <= n) {
        c16_u8 P[16];
        c16_u16 W4[16], W8[16];
        c16_u32 o = off + x * 2, left = 0xffff, i;
        __m256i w4, w8, out = _mm256_setzero_si256();
        __m128i pv;

        for (i = 0; i < 16; i++) {
            c16_u32 k = mask[x + i];
            P[i] = p->pid[k];
            W4[i] = p->w4[k];
            W8[i] = p->w8[k];
        }
        pv = _mm_loadu_si128((const __m128i *)P);
        w4 = _mm256_loadu_si256((const __m256i *)W4);
        w8 = _mm256_loadu_si256((const __m256i *)W8);
        while (left) {
            c16_u32 id = P[__builtin_ctz(left)], ov = p->pair_over[id];
            __m128i eq = _mm_cmpeq_epi8(pv, _mm_set1_epi8((char)id));
            __m256i d = _mm256_loadu_si256((const __m256i *)(p->pair_base[id] + o));
            __m256i c = ov ? _mm256_loadu_si256((const __m256i *)(ov + o)) : d;
            __m256i v = blend565_x16(c, d, w4, w8);
            c16_u32 m = (c16_u32)_mm_movemask_epi8(eq);

            out = m == 0xffff ? v : _mm256_blendv_epi8(out, v, _mm256_cvtepi8_epi16(eq));
            left &= ~m;
        }
        _mm256_storeu_si256((__m256i *)(dst + x), out);
        x += 16;
    }
    if (x < n)
        row_c(dst + x, mask + x, n - x, off + x * 2, l);
}

#endif
