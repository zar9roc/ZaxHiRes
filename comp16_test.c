/*
 * comp16_test.c - checks row_avx2 against row_c (comp16.h) on random data.
 * 32-bit build, because the layer tables hold 32-bit addresses:
 *     zig cc -target x86-linux-musl -O2 -o comp16_test comp16_test.c && ./comp16_test
 * row_c itself is checked against the game in-game with /CompVerify.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "comp16.h"

#define TEX    4096                     /* pixels per layer texture */
#define LAYERS 256

static c16_u32 rnd_state = 12345;

static c16_u32 rnd(void)
{
    rnd_state = rnd_state * 1103515245u + 12345u;
    return rnd_state >> 8;
}

int main(void)
{
    static c16_u16 tex[LAYERS][TEX], ovl[LAYERS][TEX], out_c[TEX], out_v[TEX];
    static c16_u8 mask[TEX];
    c16_u32 base[LAYERS], over[LAYERS], weight[LAYERS];
    static C16Prep prep;
    Layers l = { base, over, weight, &prep };
    long rows = 0, bad = 0;
    int i, x, round;

    for (i = 0; i < LAYERS; i++)
        for (x = 0; x < TEX; x++) {
            tex[i][x] = (c16_u16)rnd();
            ovl[i][x] = (c16_u16)rnd();
        }

    for (round = 0; round < 120000; round++) {
        c16_u32 n = 1 + rnd() % 300, off = (rnd() % (TEX - 400)) * 2;
        int run = rnd() % 4;

        if (round % 3 == 2) {
            /* few textures combined freely: one base with several overlays */
            for (i = 0; i < LAYERS; i++) {
                base[i] = (c16_u32)(size_t)tex[rnd() % 3];
                over[i] = rnd() % 4 ? (c16_u32)(size_t)ovl[rnd() % 3] : 0;
                weight[i] = rnd() % 256;
            }
            for (x = 0; x < (int)n; x++)
                mask[x] = (c16_u8)(run ? x / (1 + run) : rnd());
        } else if (round % 3) {
            /* the game's layout: bands of 26 indices, base A / overlay B with
               rising weights, then B / C, ...; some base-only entries */
            for (i = 0; i < LAYERS; i++) {
                int band = i / 26, step = i % 26;
                base[i] = (c16_u32)(size_t)tex[band];
                over[i] = step || rnd() % 2 ? (c16_u32)(size_t)tex[band + 1] : 0;
                weight[i] = over[i] ? (c16_u32)(step * 10 + round % 10) % 256 : 0;
            }
            /* masks: a random walk over the indices, crossing band edges */
            mask[0] = (c16_u8)rnd();
            for (x = 1; x < (int)n; x++)
                mask[x] = (c16_u8)(mask[x - 1] + (int)(rnd() % (run * 3 + 3)) - (run * 3 + 2) / 2);
        } else {
            /* every weight 0..2047 is covered over the rounds, plus random ones */
            for (i = 0; i < LAYERS; i++) {
                base[i] = (c16_u32)(size_t)tex[i];
                over[i] = rnd() % 3 ? (c16_u32)(size_t)ovl[i] : 0;
                weight[i] = i < 8 ? (c16_u32)((round * 4 + i) % 2048) : rnd() % 2048;
            }
            /* masks: long runs of one index, short runs, or noise */
            for (x = 0; x < (int)n; x++)
                mask[x] = run ? (c16_u8)((x / (1 + run * 8) + round) % 8) : (c16_u8)rnd();
        }

        c16_prepare(base, over, weight, &prep);
        memset(out_c, 0, sizeof out_c);
        memset(out_v, 0, sizeof out_v);
        row_c(out_c, mask, n, off, &l);
        row_avx2(out_v, mask, n, off, &l);
        rows++;
        for (x = 0; x < TEX; x++)
            if (out_c[x] != out_v[x] && bad++ < 10)
                printf("round %d n %u x %d idx %u w %u: c %04x avx2 %04x\n", round, n, x,
                       x < (int)n ? mask[x] : 0, x < (int)n ? weight[mask[x]] : 0,
                       out_c[x], out_v[x]);
    }
    printf("%ld rows, %ld mismatching pixels\n", rows, bad);
    return bad != 0;
}
