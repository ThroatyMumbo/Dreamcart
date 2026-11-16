#ifndef GFX_SH4_H
#define GFX_SH4_H

// The HLE's SH-4 matrix unit: XMTRX in the FPU's back bank, and ftrv on fv0 (fr0-fr3)

// ftrv reads XMTRX column-major, so the row-major m lands as its transpose, v * m
static void xmtrx_load(const float m[4][4]) {
    const float *p = &m[0][0];
    __asm__ volatile("frchg\n\t"
                     "fmov.s @%0+,fr0\n\tfmov.s @%0+,fr1\n\tfmov.s @%0+,fr2\n\tfmov.s @%0+,fr3\n\t"
                     "fmov.s @%0+,fr4\n\tfmov.s @%0+,fr5\n\tfmov.s @%0+,fr6\n\tfmov.s @%0+,fr7\n\t"
                     "fmov.s @%0+,fr8\n\tfmov.s @%0+,fr9\n\tfmov.s @%0+,fr10\n\tfmov.s @%0+,fr11\n\t"
                     "fmov.s @%0+,fr12\n\tfmov.s @%0+,fr13\n\tfmov.s @%0+,fr14\n\tfmov.s @%0+,fr15\n\t"
                     "frchg"
                     : "+r"(p) : "m"(*(const float(*)[16])m));
}

// fv0 = fv0 * XMTRX, on four floats declared as register fr0-fr3
#define FTRV_FV0(f0, f1, f2, f3) __asm__("ftrv xmtrx,fv0" : "+f"(f0), "+f"(f1), "+f"(f2), "+f"(f3))

// Row i of a, through XMTRX = b, is row i of a * b
static void mul44(float r[4][4], const float a[4][4], const float b[4][4]) {
    float t[4][4];
    xmtrx_load(b);
    for (int i = 0; i < 4; i++) {
        register float f0 __asm__("fr0") = a[i][0], f1 __asm__("fr1") = a[i][1];
        register float f2 __asm__("fr2") = a[i][2], f3 __asm__("fr3") = a[i][3];
        FTRV_FV0(f0, f1, f2, f3);
        t[i][0] = f0, t[i][1] = f1, t[i][2] = f2, t[i][3] = f3;
    }
    __builtin_memcpy(r, t, sizeof t);
}

#endif
