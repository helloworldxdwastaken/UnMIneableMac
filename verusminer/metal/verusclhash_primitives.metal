// verusclhash_primitives.metal — the two remaining "trivial" primitives
// that verusclhash needs on the GPU: mulhrs_epi16 (signed 16-bit
// fixed-point multiply with rounding) and precompReduction64 (the GF
// reduction that closes the verusclhash pipeline).
//
// With these + clmul64.metal + the three haraka kernels, every cryptographic
// primitive verusclhash_sv2_2 needs is on the GPU. The only remaining work
// is the 32-iteration selector loop body, which is straight translation of
// the C reference (8 switch cases, each combining the primitives).

#include <metal_stdlib>
using namespace metal;

// ============================================================
// mulhrs_epi16 — Intel SSSE3 packed signed 16-bit "multiply high
// with rounding and saturation". Direct port of
// canonical/verus_clhash_portable.cpp _mm_mulhrs_epi16_emu lines 106-132.
//
//   r[i] = (int16_t)(((int32_t)a[i] * b[i] + 0x4000) >> 15)
//
// Treats each 16-byte input as 8 packed int16. Lane-independent.
// ============================================================
inline void mulhrs_8x(thread short *out, thread const short *a, thread const short *b) {
    for (int i = 0; i < 8; i++) {
        int prod = (int)a[i] * (int)b[i];
        out[i] = (short)((prod + 0x4000) >> 15);
    }
}

kernel void mulhrs_kernel(
    device const uchar *inputs  [[buffer(0)]],   // N pairs of (a:16B, b:16B) = 32B per pair
    device uchar       *outputs [[buffer(1)]],   // N × 16B results
    uint gid [[thread_position_in_grid]])
{
    // Load 8 int16 from a, 8 from b
    short a[8], b[8], r[8];
    device const uchar *src = inputs + gid * 32;
    for (int i = 0; i < 8; i++) {
        a[i] = (short)((uint)src[i*2] | ((uint)src[i*2+1] << 8));
    }
    for (int i = 0; i < 8; i++) {
        int o = 16 + i * 2;
        b[i] = (short)((uint)src[o] | ((uint)src[o+1] << 8));
    }

    mulhrs_8x(r, a, b);

    device uchar *dst = outputs + gid * 16;
    for (int i = 0; i < 8; i++) {
        dst[i*2]     = (uchar)(  r[i]        & 0xff);
        dst[i*2 + 1] = (uchar)( ((uint)r[i] >> 8) & 0xff);
    }
}

// ============================================================
// precompReduction64 — GF(2^128) modular reduction down to 64 bits.
//
// Direct port of verus_clhash_portable.cpp lines 315-325.
//
//   C = 0x1B (poly x^4 + x^3 + x + 1, set as low u64 of 128-bit vec)
//   Q2 = clmulepi64(A, C, 0x01)   ; multiplies A.hi × C.lo
//   Q3 = shuffle_epi8(LUT16, srli_si128(Q2, 8))  ; LUT lookup on Q2.hi bytes
//   Q4 = A XOR Q2
//   final = Q3 XOR Q4              ; high 64 bits are garbage; low = our hash
//
// The 16-byte LUT comes from _mm_setr_epi8 in the original code with values
// {0, 27, 54, 45, 108, 119, 90, 65, 216, 195, 238, 245, 180, 175, 130, 153}.
// As bytes (216=0xd8, 195=0xc3, 238=0xee, 245=0xf5, 180=0xb4, 175=0xaf,
// 130=0x82, 153=0x99) the table is the GF(2)-multiply-by-0x1B values for
// each possible nibble — basically a precomputed reduction table.
// ============================================================
constant uchar PRECOMP_LUT16[16] = {
    0x00, 0x1b, 0x36, 0x2d, 0x6c, 0x77, 0x5a, 0x41,
    0xd8, 0xc3, 0xee, 0xf5, 0xb4, 0xaf, 0x82, 0x99
};

// clmul64 — duplicated from clmul64.metal so this file is self-contained.
// Kernels that want both primitives + clmul should include this once and
// reuse the helper.
inline void clmul64_gpu(uint64_t a, uint64_t b,
                        thread uint64_t &r0, thread uint64_t &r1) {
    const uint s = 4;
    const uint64_t smask = (1ul << s) - 1ul;
    uint64_t u[16];
    u[0] = 0; u[1] = b;
    for (uint i = 2; i < (1u << s); i += 2) {
        u[i]     = u[i >> 1] << 1;
        u[i + 1] = u[i] ^ b;
    }
    r0 = u[a & smask];
    r1 = 0;
    for (uint i = s; i < 64; i += s) {
        uint64_t tmp = u[(a >> i) & smask];
        r0 ^= tmp << i;
        r1 ^= tmp >> (64u - i);
    }
    uint64_t m = 0xEEEEEEEEEEEEEEEEul;
    for (uint i = 1; i < s; i++) {
        uint64_t tmp = (a & m) >> i;
        m &= (m << 1);
        uint64_t ifmask = (uint64_t)0 - (uint64_t)((b >> (64u - i)) & 1ul);
        r1 ^= (tmp & ifmask);
    }
}

// srli_si128 by 8 (right shift by 8 bytes). Result high 8 bytes = 0,
// low 8 bytes = original high 8 bytes.
inline void srli_si128_by_8(thread uchar *out, thread const uchar *in) {
    for (int i = 0; i < 8; i++)  out[i] = in[i + 8];
    for (int i = 8; i < 16; i++) out[i] = 0;
}

// shuffle_epi8(LUT, b): for each byte b[i], if MSB set output 0, else
// output LUT[b[i] & 0x0f].
inline void shuffle_epi8_lut(thread uchar *out, constant uchar *lut,
                             thread const uchar *b) {
    for (int i = 0; i < 16; i++) {
        uchar bi = b[i];
        out[i] = (bi & 0x80) ? 0 : lut[bi & 0x0f];
    }
}

// Full precompReduction64: takes 16-byte A, returns 16 bytes where the
// low 8 are the hash output and the high 8 are garbage (per the CPU
// reference comment).
inline void precomp_reduction64(thread uchar *out, thread const uchar *A) {
    // Load A as two u64 LE
    uint64_t a_lo = 0, a_hi = 0;
    for (int i = 0; i < 8; i++) {
        a_lo |= ((uint64_t)A[i])     <<  (8 * i);
        a_hi |= ((uint64_t)A[i + 8]) <<  (8 * i);
    }

    // Q2 = clmulepi64(A, C, 0x01) → multiply A's high u64 with C's low u64
    // C.lo = 0x1B, C.hi = 0 (set by _mm_cvtsi64_si128_emu)
    uint64_t q2_lo, q2_hi;
    clmul64_gpu(a_hi, 0x1Bul, q2_lo, q2_hi);

    // Pack Q2 as 16 bytes
    uchar q2[16];
    for (int i = 0; i < 8; i++) {
        q2[i]     = (uchar)((q2_lo >> (8 * i)) & 0xff);
        q2[i + 8] = (uchar)((q2_hi >> (8 * i)) & 0xff);
    }

    // Q3 = shuffle_epi8(LUT, srli_si128(Q2, 8))
    uchar q2_shifted[16];
    srli_si128_by_8(q2_shifted, q2);
    uchar q3[16];
    shuffle_epi8_lut(q3, PRECOMP_LUT16, q2_shifted);

    // Q4 = A XOR Q2 ; final = Q3 XOR Q4
    for (int i = 0; i < 16; i++) {
        uchar q4 = A[i] ^ q2[i];
        out[i] = q3[i] ^ q4;
    }
}

kernel void precomp_reduction64_kernel(
    device const uchar *inputs  [[buffer(0)]],   // N × 16B
    device uchar       *outputs [[buffer(1)]],   // N × 16B (low 8 = hash)
    uint gid [[thread_position_in_grid]])
{
    uchar A[16], out[16];
    device const uchar *src = inputs + gid * 16;
    for (int i = 0; i < 16; i++) A[i] = src[i];
    precomp_reduction64(out, A);
    device uchar *dst = outputs + gid * 16;
    for (int i = 0; i < 16; i++) dst[i] = out[i];
}
