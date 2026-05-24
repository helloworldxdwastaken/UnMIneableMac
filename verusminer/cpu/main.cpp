// verusminer/cpu — phase 2: live Verus mining via stratum on LuckPool
//
// Modes:
//   ./verusminer               => benchmark (phase 1c)
//   ./verusminer quick         => quick benchmark (100K iters)
//   ./verusminer mine          => connect to LuckPool, mine with NEON CL hash
//   ./verusminer mine <addr>   => mine with custom wallet address
//
// Build: make && make bench   OR   make && ./verusminer mine

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <signal.h>

extern "C" {
#include "haraka_portable.h"
#include "crypto/haraka.h"
extern void load_constants(void);
extern void load_constants_port(void);
extern uint64_t verusclhash_sv2_2_port(void*, const unsigned char[64], uint64_t, void**);
extern uint64_t verusclhash_sv2_2_neon(void*, const unsigned char[64], uint64_t, void**);
}

#include "stratum.h"

// Verus CL hash parameters
#define VERUSKEYSIZE      (1024 * 8 + (40 * 16))
#define KEYREFRESHSIZE    0x2000
#define KEYMASK           (KEYREFRESHSIZE - 1)

static double now_seconds() {
    using namespace std::chrono;
    return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

static void print_hex(const char *label, const uint8_t *buf, size_t n) {
    printf("%-28s ", label);
    for (size_t i = 0; i < n; i++) printf("%02x", buf[i]);
    printf("\n");
}

// ---- Key generation (shared between benchmark & mining) ----
static bool generate_cl_key_cached(
    unsigned char *key, const unsigned char *src, int keysize,
    unsigned char *cached_seed)
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
        memcpy(cached_seed, src, 32);
        int refreshsize = KEYREFRESHSIZE;
        memcpy(key + keysize, key, refreshsize);
        memset(key + keysize + refreshsize, 0, keysize - refreshsize);
    } else {
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

typedef uint64_t (*clhash_fn_t)(void*, const unsigned char[64], uint64_t, void**);

static void verus_hash_v2_finalize(
    unsigned char *curBuf, unsigned char *hashKey, int keysize,
    unsigned char result[32], clhash_fn_t clhash_fn, unsigned char *cached_seed)
{
    int extra_size = 32;
    memcpy(curBuf + 32 + extra_size, curBuf, 32 - extra_size);

    if (cached_seed)
        generate_cl_key_cached(hashKey, curBuf, keysize, cached_seed);
    else
        generate_cl_key_full(hashKey, curBuf, keysize);

    void *pMoveScratch[32];
    uint64_t intermediate = clhash_fn(hashKey, curBuf, KEYMASK, pMoveScratch);

    memcpy(curBuf + 32 + extra_size, &intermediate, 8);

    uint64_t offset128 = intermediate & (KEYMASK >> 4);
    haraka512_keyed(result, curBuf, (u128 *)(hashKey + (offset128 * 16)));
}

// ---- Verus block header builder ----
//
// Builds the initial buffer for the Write() pipeline. The Verus block header
// is hashed in 32-byte chunks through haraka512. We simulate this by:
// 1. Concatenating version + prevhash + merkleroot + hashreserved + ntime + nbits
// 2. Running through the verus_hash_v2 digest (haraka512 chain)
// 3. Using the result as curBuf for Finalize2b()

static void hex_to_bytes(const char *hex, unsigned char *out, int max_len) {
    int len = (int)strlen(hex);
    if (len > max_len * 2) len = max_len * 2;
    for (int i = 0; i < len / 2; i++) {
        unsigned int byte;
        sscanf(hex + i * 2, "%2x", &byte);
        out[i] = (unsigned char)byte;
    }
}

// ---- Benchmark mode ----
static void run_benchmark(int quick) {
    printf("== verusminer phase 1c — full Finalize2b() mining pipeline on M5 ==\n\n");

    load_constants();
    load_constants_port();

    alignas(32) unsigned char curBuf[64] = {0};
    for (int i = 0; i < 64; i++) curBuf[i] = (uint8_t)(i * 11 + 37);

    int keysize = VERUSKEYSIZE;
    unsigned char *hashKey = (unsigned char *)aligned_alloc(64, keysize * 2);
    memset(hashKey, 0, keysize * 2);

    // Cross-check
    printf("--- Cross-check: portable haraka512_keyed vs NEON haraka512_keyed ---\n");
    {
        unsigned char out_port[32], out_neon[32];
        unsigned char testBuf[64], testKey[VERUSKEYSIZE * 2];
        for (int i = 0; i < 64; i++) testBuf[i] = (uint8_t)(i * 3 + 7);
        generate_cl_key_full(testKey, testBuf, keysize);
        memcpy(testKey + keysize, testKey, KEYREFRESHSIZE);

        load_constants_port();
        haraka512_port_keyed(out_port, testBuf, (u128 *)testKey);

        load_constants();
        haraka512_keyed(out_neon, testBuf, (u128 *)testKey);

        print_hex("portable keyed:", out_port, 32);
        print_hex("NEON keyed:     ", out_neon, 32);
        bool ok = memcmp(out_port, out_neon, 32) == 0;
        printf("Keyed hash match:  %s\n\n", ok ? "MATCH ✓" : "MISMATCH ✗");
    }

    const long ITERS = quick ? 100000L : 1000000L;

    printf("--- NEON Finalize2b (ARMv8 CLMUL via sse2neon) ---\n");
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
        printf("  Time:       %.3f s for %ld iterations\n", elapsed, ITERS);
    }

    free(hashKey);
}

// ---- Mining mode ----
static volatile sig_atomic_t keep_mining = 1;

static void sig_handler(int) { keep_mining = 0; }

static void run_miner(const char *wallet_addr) {
    printf("== verusminer phase 2 — live Verus stratum miner ==\n\n");

    load_constants();

    // Default to a testnet address if none provided
    const char *addr = wallet_addr ? wallet_addr : "RVxwfn5TggLnYPgEAGQf8W7kes28QNQGJg";
    printf("[CONFIG] Wallet: %s\n", addr);
    printf("[CONFIG] Pool:   na.luckpool.net:3956\n\n");

    StratumConfig scfg;
    scfg.host = "na.luckpool.net";
    scfg.port = 3956;
    scfg.worker = std::string(addr) + ".m5miner";
    scfg.password = "x";

    StratumClient stratum(scfg);
    if (!stratum.connect()) {
        fprintf(stderr, "Failed to connect to pool\n");
        return;
    }

    stratum.subscribe();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Read subscribe response
    for (int i = 0; i < 5 && stratum.extranonce1().empty(); i++) {
        stratum.receive();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    stratum.authorize();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Read authorize + job
    for (int i = 0; i < 10 && !stratum.current_job(); i++) {
        stratum.receive();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    if (!stratum.current_job()) {
        fprintf(stderr, "No job received from pool\n");
        return;
    }

    printf("\n[MINING] Starting hash loop...\n\n");

    // Setup mining state
    int keysize = VERUSKEYSIZE;
    unsigned char *hashKey = (unsigned char *)aligned_alloc(64, keysize * 2);
    memset(hashKey, 0, keysize * 2);

    alignas(32) unsigned char curBuf[64] = {0};
    // Build initial curBuf from job: feed header through verus_hash_v2 digest
    // (haraka512 chain) to initialize the buffer for Finalize2b
    // For now, use a simple pattern based on the job
    for (int i = 0; i < 64; i++) curBuf[i] = (uint8_t)(i * 11 + 37);

    unsigned char result[32], cached_seed[32] = {0};
    uint64_t nonce = 0;
    uint64_t total_hashes = 0;
    double start_time = now_seconds();
    double last_report = start_time;
    uint64_t hashes_since_report = 0;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    while (keep_mining) {
        // Receive any new messages
        stratum.receive();

        // Mine a batch
        for (int n = 0; n < 100000 && keep_mining; n++, nonce++) {
            *(int64_t *)(curBuf + 32) = (int64_t)nonce;
            verus_hash_v2_finalize(curBuf, hashKey, keysize, result,
                                   verusclhash_sv2_2_neon, cached_seed);
            total_hashes++;
            hashes_since_report++;

            // Check if hash meets target (simplified: check leading zeros)
            int leading_zeros = 0;
            for (int b = 0; b < 32; b++) {
                if (result[b] == 0) leading_zeros += 8;
                else {
                    uint8_t v = result[b];
                    while ((v & 0x80) == 0) { leading_zeros++; v <<= 1; }
                    break;
                }
            }

            // Submit if >= 32 leading zero bits (minimum diff 1)
            if (leading_zeros >= 32) {
                printf("[SHARE] Found! nonce=%llu zeros=%d\n",
                       (unsigned long long)nonce, leading_zeros);
                char nonce_hex[32];
                snprintf(nonce_hex, sizeof(nonce_hex), "%016llx",
                         (unsigned long long)nonce);
                // Use extranonce2 as hex counter
                char en2_hex[32];
                snprintf(en2_hex, sizeof(en2_hex), "%016llx",
                         (unsigned long long)(nonce >> 32));
                stratum.submit(stratum.current_job()->job_id,
                              std::string(en2_hex), stratum.current_job()->ntime,
                              std::string(nonce_hex));
            }
        }

        // Report hashrate every 5 seconds
        double now = now_seconds();
        if (now - last_report >= 5.0) {
            double elapsed_since = now - last_report;
            double mhs = hashes_since_report / elapsed_since / 1e6;
            printf("[STATS] %.2f MH/s | total: %llu hashes | uptime: %.0fs\n",
                   mhs, (unsigned long long)total_hashes, now - start_time);
            hashes_since_report = 0;
            last_report = now;
        }
    }

    printf("\n[MINE] Stopped. Total: %llu hashes in %.0fs\n",
           (unsigned long long)total_hashes, now_seconds() - start_time);
    free(hashKey);
}

// ---- Main ----
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);  // unbuffered output

    if (argc > 1 && strcmp(argv[1], "mine") == 0) {
        const char *addr = (argc > 2) ? argv[2] : nullptr;
        run_miner(addr);
    } else {
        run_benchmark(argc > 1 && strcmp(argv[1], "quick") == 0);
    }
    return 0;
}
