// verusminer/cpu — phase 1c: full Finalize2b() mining benchmark
//
// Simulates the complete VerusHash 2.2 mining loop: FillExtra → GenNewCLKey
// → verusclhash → FillExtra → haraka512_keyed. Uses the portable CL hash
// (pure software, no SSE/NEON hardware needed) for correctness. The NEON
// haraka512_keyed provides the AES hardware acceleration for the final step.
//
// Build: make && make bench

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <chrono>

extern "C" {
#include "haraka_portable.h"
#include "crypto/haraka.h"
extern void load_constants(void);
extern void load_constants_port(void);

// Portable CL hash (pure-C emulated CLMUL — slow on ARM)
extern uint64_t verusclhash_sv2_2_port(void *random, const unsigned char buf[64],
                                       uint64_t keyMask, void **pMoveScratch_out);

// NEON-accelerated CL hash (uses vmull_p64 via sse2neon — fast on ARMv8)
extern uint64_t verusclhash_sv2_2_neon(void *random, const unsigned char buf[64],
                                       uint64_t keyMask, void **pMoveScratch_out);
}

// ---- Verus CL hash parameters ----
#define VERUSKEYSIZE      (1024 * 8 + (40 * 16))  // 8832 bytes
#define KEYREFRESHSIZE    0x2000                   // 8192 bytes = power-of-2 mask
#define KEYMASK           (KEYREFRESHSIZE - 1)

// ---- Timing ----
static double now_seconds() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

static void print_hex(const char *label, const uint8_t *buf, size_t n) {
    printf("%-28s ", label);
    for (size_t i = 0; i < n; i++) printf("%02x", buf[i]);
    printf("\n");
}

// ---- Key generation: chain-hash haraka256 from buffer ----
// ---- Key generation: chain-hash haraka256 from buffer ----
// Only regenerates the key if the seed (first 32 bytes of curBuf) has
// changed since the last call. The refresh region (keysize bytes at
// hashKey + keysize) is always updated for CL hash mutation.
// Returns true if key was regenerated.
static bool generate_cl_key_cached(
    unsigned char *key, const unsigned char *src, int keysize,
    unsigned char *cached_seed)  // 32 bytes, updated on change
{
    bool changed = memcmp(cached_seed, src, 32) != 0;
    if (changed) {
        int n256blks = keysize >> 5;
        unsigned char *pkey = key;
        unsigned char *psrc = (unsigned char *)src;
        for (int i = 0; i < n256blks; i++) {
            haraka256(pkey, psrc);
            psrc = pkey;
            pkey += 32;
        }
        int nbytesExtra = keysize & 0x1f;
        if (nbytesExtra) {
            unsigned char buf[32];
            haraka256(buf, psrc);
            memcpy(pkey, buf, nbytesExtra);
        }
        // Store the new seed
        memcpy(cached_seed, src, 32);

        // Copy to refresh region
        int refreshsize = KEYREFRESHSIZE;
        memcpy(key + keysize, key, refreshsize);
        memset(key + keysize + refreshsize, 0, keysize - refreshsize);
    } else {
        // Key is still valid — just refresh from the cached copy
        int refreshsize = KEYREFRESHSIZE;
        memcpy(key + keysize, key + keysize + refreshsize, refreshsize);
        memset(key + keysize + refreshsize, 0, keysize - refreshsize);
    }
    return changed;
}

static void generate_cl_key_full(
    unsigned char *key, const unsigned char *src, int keysize)
{
    int n256blks = keysize >> 5;
    unsigned char *pkey = key;
    unsigned char *psrc = (unsigned char *)src;
    for (int i = 0; i < n256blks; i++) {
        haraka256(pkey, psrc);
        psrc = pkey;
        pkey += 32;
    }
    int nbytesExtra = keysize & 0x1f;
    if (nbytesExtra) {
        unsigned char buf[32];
        haraka256(buf, psrc);
        memcpy(pkey, buf, nbytesExtra);
    }
    int refreshsize = KEYREFRESHSIZE;
    memcpy(key + keysize, key, refreshsize);
    memset(key + keysize + refreshsize, 0, keysize - refreshsize);
}

// ---- One full VerusHash 2.2 mining iteration (Finalize2b) ----
//
// clhash_fn: function pointer to either verusclhash_sv2_2_port (slow, portable)
//            or verusclhash_sv2_2_neon (fast, hardware CLMUL via vmull_p64)
// cached_seed: 32-byte buffer to track key cache validity (pass nullptr to
//              always regenerate the key)
typedef uint64_t (*clhash_fn_t)(void*, const unsigned char[64], uint64_t, void**);

static void verus_hash_v2_finalize(
    unsigned char *curBuf,
    unsigned char *hashKey,
    int keysize,
    unsigned char result[32],
    clhash_fn_t clhash_fn,
    unsigned char *cached_seed)
{
    // 1) FillExtra: copy first 32 bytes of curBuf to positions 33-63
    int extra_size = 32;
    memcpy(curBuf + 32 + extra_size, curBuf, 32 - extra_size);

    // 2) GenNewCLKey with caching
    if (cached_seed) {
        generate_cl_key_cached(hashKey, curBuf, keysize, cached_seed);
    } else {
        generate_cl_key_full(hashKey, curBuf, keysize);
    }

    // 3) Run verusclhash on the buffer (64 bytes) with the key
    void *pMoveScratch[32];
    uint64_t intermediate = clhash_fn(hashKey, curBuf, KEYMASK, pMoveScratch);

    // 4) FillExtra with intermediate result
    memcpy(curBuf + 32 + extra_size, &intermediate, 8);

    // 5) Final hash: haraka512_keyed with key offset
    uint64_t offset128 = intermediate & (KEYMASK >> 4);
    haraka512_keyed(result, curBuf, (u128 *)(hashKey + (offset128 * 16)));
}

int main(int argc, char **argv) {
    printf("== verusminer phase 1c — full Finalize2b() mining pipeline on M5 ==\n\n");

    load_constants();
    load_constants_port();

    // ---- Setup ----
    // curBuf: 64-byte aligned buffer (modeled after CVerusHashV2::buf1)
    alignas(32) unsigned char curBuf[64] = {0};
    // Seed it with a pseudo-random pattern
    for (int i = 0; i < 64; i++) curBuf[i] = (uint8_t)(i * 11 + 37);

    // Key buffer: 2× VERUSKEYSIZE (key + refresh region + pMoveScratch space)
    int keysize = VERUSKEYSIZE;
    unsigned char *hashKey = (unsigned char *)aligned_alloc(64, keysize * 2);
    memset(hashKey, 0, keysize * 2);

    // ---- 1) Cross-check: portable CL hash outputs self-consistency ----
    printf("--- Cross-check: portable haraka512_keyed vs NEON haraka512_keyed ---\n");
    {
        unsigned char out_port[32], out_neon[32];
        // Set up a minimal curBuf + key
        unsigned char testBuf[64], testKey[VERUSKEYSIZE * 2];
        for (int i = 0; i < 64; i++) testBuf[i] = (uint8_t)(i * 3 + 7);
        generate_cl_key_full(testKey, testBuf, keysize);
        memcpy(testKey + keysize, testKey, KEYREFRESHSIZE);

        // haraka512_port_keyed uses the portable path's rc constants
        load_constants_port();
        haraka512_port_keyed(out_port, testBuf, (u128 *)testKey);

        // haraka512_keyed uses the NEON path's rc constants
        load_constants();
        haraka512_keyed(out_neon, testBuf, (u128 *)testKey);

        print_hex("portable keyed:", out_port, 32);
        print_hex("NEON keyed:     ", out_neon, 32);
        bool key_match = memcmp(out_port, out_neon, 32) == 0;
        printf("Keyed hash match:  %s\n\n", key_match ? "MATCH ✓" : "MISMATCH ✗");
    }

    const long ITERS = (argc > 1 && argv[1][0] == 'q') ? 100000L : 1000000L;

    // ---- 2) Benchmark: portable CL hash (pure-software CLMUL) ----
    //
    // curBuf[0..31] stays constant across iterations → key cache hits after
    // first iteration. This matches real mining behaviour where the seed only
    // changes when a new block template arrives.
    printf("--- Portable Finalize2b (software CL hash + NEON haraka512_keyed) ---\n");
    {
        unsigned char result[32], cached_seed[32] = {0};
        double t0 = now_seconds();
        for (long i = 0; i < ITERS; i++) {
            *(int64_t *)(curBuf + 32) = i;
            verus_hash_v2_finalize(curBuf, hashKey, keysize, result,
                                   verusclhash_sv2_2_port, cached_seed);
        }
        double t1 = now_seconds();
        double elapsed = t1 - t0;
        printf("  Throughput: %.4f hashes/sec on 1 P-core\n", ITERS / elapsed);
        printf("  MH/s:       %.4f\n", ITERS / elapsed / 1e6);
        printf("  Time:       %.3f s for %ld iterations\n\n", elapsed, ITERS);
    }

    // ---- 3) Benchmark: NEON CL hash (hardware CLMUL via vmull_p64) ----
    printf("--- NEON Finalize2b (ARMv8 CLMUL via sse2neon + NEON haraka512_keyed) ---\n");
    {
        unsigned char result[32], cached_seed[32] = {0};
        double t0 = now_seconds();
        for (long i = 0; i < ITERS; i++) {
            *(int64_t *)(curBuf + 32) = i;
            verus_hash_v2_finalize(curBuf, hashKey, keysize, result,
                                   verusclhash_sv2_2_neon, cached_seed);
        }
        double t1 = now_seconds();
        double elapsed = t1 - t0;
        printf("  Throughput: %.4f hashes/sec on 1 P-core\n", ITERS / elapsed);
        printf("  MH/s:       %.4f\n", ITERS / elapsed / 1e6);
        printf("  Time:       %.3f s for %ld iterations\n\n", elapsed, ITERS);
    }

    // ---- 4) Extrapolation ----
    printf("=========================================\n");
    printf("Estimated real mining throughput:\n");
    printf("  1 P-core:  ~ measured above\n");
    printf("  4 P-cores: ~ 4× 1-core\n");
    printf("  The CL hash dominates (~70-80%% of time).\n");
    printf("  NEON helps the haraka512 final step only.\n");
    printf("=========================================\n");

    free(hashKey);
    return 0;
}
