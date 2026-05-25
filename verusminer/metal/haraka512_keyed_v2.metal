// haraka512_keyed_v2.metal — same structure as haraka512_v2.metal but
// round keys come from a runtime device buffer instead of the static RC.
// This is the variant verusclhash calls per-iteration: the "round
// constants" are actually a 640-byte (40 × u128) buffer derived from the
// CL hash key seed.
//
// Differences from haraka512_v2.metal:
//   - No constant RC[160] table
//   - aesenc_tt_keyed() takes `device const uint *rk` instead of `constant`
//   - Kernel takes a 3rd input buffer: round_keys (160 uint32 LE)
//
// Validated against haraka_portable.c's haraka512_port_keyed.

#include <metal_stdlib>
using namespace metal;

constant uchar SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

inline uchar gf2(uchar x) {
    return (uchar)((((int)x) << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

inline uint t_row(uchar b, int row) {
    uchar s   = SBOX[b];
    uchar f2s = gf2(s);
    uchar f3s = f2s ^ s;
    uchar c0, c1, c2, c3;
    if (row == 0)      { c0=f2s; c1=s;   c2=s;   c3=f3s; }
    else if (row == 1) { c0=f3s; c1=f2s; c2=s;   c3=s;   }
    else if (row == 2) { c0=s;   c1=f3s; c2=f2s; c3=s;   }
    else               { c0=s;   c1=s;   c2=f3s; c3=f2s; }
    return (uint)c0 | ((uint)c1 << 8) | ((uint)c2 << 16) | ((uint)c3 << 24);
}

// aesenc with round keys from `device` address space (vs `constant` in
// the static-RC variant). Identical body otherwise — Metal can't overload
// across address spaces without templates, so we duplicate the function.
inline void aesenc_tt_keyed(thread uint *s, device const uint *rk) {
    uint x0 = s[0], x1 = s[1], x2 = s[2], x3 = s[3];
    uint y0, y1, y2, y3;

    y0 = t_row((uchar)( x0        & 0xff), 0);
    y1 = t_row((uchar)( x1        & 0xff), 0);
    y2 = t_row((uchar)( x2        & 0xff), 0);
    y3 = t_row((uchar)( x3        & 0xff), 0);

    y0 ^= t_row((uchar)((x1 >>  8) & 0xff), 1);
    y1 ^= t_row((uchar)((x2 >>  8) & 0xff), 1);
    y2 ^= t_row((uchar)((x3 >>  8) & 0xff), 1);
    y3 ^= t_row((uchar)((x0 >>  8) & 0xff), 1);

    y0 ^= t_row((uchar)((x2 >> 16) & 0xff), 2);
    y1 ^= t_row((uchar)((x3 >> 16) & 0xff), 2);
    y2 ^= t_row((uchar)((x0 >> 16) & 0xff), 2);
    y3 ^= t_row((uchar)((x1 >> 16) & 0xff), 2);

    y0 ^= t_row((uchar)( x3 >> 24       ), 3);
    y1 ^= t_row((uchar)( x0 >> 24       ), 3);
    y2 ^= t_row((uchar)( x1 >> 24       ), 3);
    y3 ^= t_row((uchar)( x2 >> 24       ), 3);

    s[0] = y0 ^ rk[0];
    s[1] = y1 ^ rk[1];
    s[2] = y2 ^ rk[2];
    s[3] = y3 ^ rk[3];
}

inline void ld64(const device uchar *src, thread uint *w) {
    for (int i = 0; i < 16; i++) {
        uint o = i * 4;
        w[i] = (uint)src[o]
             | ((uint)src[o+1] <<  8)
             | ((uint)src[o+2] << 16)
             | ((uint)src[o+3] << 24);
    }
}

inline void st64_pair(device uchar *dst, uint lo, uint hi) {
    dst[0] =  lo        & 0xff;
    dst[1] = (lo >>  8) & 0xff;
    dst[2] = (lo >> 16) & 0xff;
    dst[3] = (lo >> 24) & 0xff;
    dst[4] =  hi        & 0xff;
    dst[5] = (hi >>  8) & 0xff;
    dst[6] = (hi >> 16) & 0xff;
    dst[7] = (hi >> 24) & 0xff;
}

inline void mix4(thread uint *s0, thread uint *s1,
                 thread uint *s2, thread uint *s3) {
    uint a0=s0[0], a1=s0[1], a2=s0[2], a3=s0[3];
    uint b0=s1[0], b1=s1[1], b2=s1[2], b3=s1[3];
    uint c0=s2[0], c1=s2[1], c2=s2[2], c3=s2[3];
    uint d0=s3[0], d1=s3[1], d2=s3[2], d3=s3[3];

    uint t0 = a0, t1 = b0, t2 = a1, t3 = b1;
    uint p0 = a2, p1 = b2, p2 = a3, p3 = b3;
    uint q0 = c0, q1 = d0, q2 = c1, q3 = d1;
    uint r0 = c2, r1 = d2, r2 = c3, r3 = d3;

    s3[0] = p0; s3[1] = r0; s3[2] = p1; s3[3] = r1;
    s0[0] = p2; s0[1] = r2; s0[2] = p3; s0[3] = r3;
    s2[0] = q2; s2[1] = t2; s2[2] = q3; s2[3] = t3;
    s1[0] = q0; s1[1] = t0; s1[2] = q1; s1[3] = t1;
}

// haraka512_keyed_kernel: same as haraka512_kernel but round keys come
// from a runtime buffer (40 × 16 = 640 bytes = 160 uint32 LE).
//
// Buffers:
//   0: inputs       — N × 64 bytes
//   1: outputs      — N × 32 bytes
//   2: round_keys   — 160 uint32 (40 × u128, packed LE per u128)
//   3: count        — atomic counter for diagnostics
kernel void haraka512_keyed_kernel(
    device const uchar *inputs     [[buffer(0)]],
    device uchar       *outputs    [[buffer(1)]],
    device const uint  *round_keys [[buffer(2)]],
    device atomic_uint *count      [[buffer(3)]],
    uint gid [[thread_position_in_grid]])
{
    uint w[16];
    ld64(inputs + gid * 64, w);

    uint s0[4] = { w[ 0], w[ 1], w[ 2], w[ 3] };
    uint s1[4] = { w[ 4], w[ 5], w[ 6], w[ 7] };
    uint s2[4] = { w[ 8], w[ 9], w[10], w[11] };
    uint s3[4] = { w[12], w[13], w[14], w[15] };

    for (uint i = 0; i < 5; i++) {
        uint base = i * 32;
        aesenc_tt_keyed(s0, round_keys + base +  0);
        aesenc_tt_keyed(s1, round_keys + base +  4);
        aesenc_tt_keyed(s2, round_keys + base +  8);
        aesenc_tt_keyed(s3, round_keys + base + 12);
        aesenc_tt_keyed(s0, round_keys + base + 16);
        aesenc_tt_keyed(s1, round_keys + base + 20);
        aesenc_tt_keyed(s2, round_keys + base + 24);
        aesenc_tt_keyed(s3, round_keys + base + 28);
        mix4(s0, s1, s2, s3);
    }

    s0[0] ^= w[ 0]; s0[1] ^= w[ 1]; s0[2] ^= w[ 2]; s0[3] ^= w[ 3];
    s1[0] ^= w[ 4]; s1[1] ^= w[ 5]; s1[2] ^= w[ 6]; s1[3] ^= w[ 7];
    s2[0] ^= w[ 8]; s2[1] ^= w[ 9]; s2[2] ^= w[10]; s2[3] ^= w[11];
    s3[0] ^= w[12]; s3[1] ^= w[13]; s3[2] ^= w[14]; s3[3] ^= w[15];

    device uchar *out = outputs + gid * 32;
    st64_pair(out +  0, s0[2], s0[3]);
    st64_pair(out +  8, s1[2], s1[3]);
    st64_pair(out + 16, s2[0], s2[1]);
    st64_pair(out + 24, s3[0], s3[1]);

    atomic_fetch_add_explicit(count, 1, memory_order_relaxed);
}
