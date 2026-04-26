// test_ipc.cc
//
// Unit tests for the chi_ipc POSIX shared-memory IPC layer.
// These run without gem5 or SystemC — they exercise:
//
//   1. ChiShmHandle lifecycle (create / tryOpen / destroy)
//   2. ChiRingBuf push/pop, empty detection, full-buffer back-pressure
//   3. Wrap-around at ring boundary (N-1 → 0 index rollover)
//   4. Round-trip of every message type (REQ, RSP, DAT, SNP) through the
//      shared layout, using two std::threads to simulate producer and consumer.
//   5. Advisory time markers (gem5_grant_ps / questa_grant_ps)
//   6. tryOpen retry loop (attaches after creator is ready)
//
// Each test uses a unique /chi_ipc_test_<pid>_<n> shm name so parallel
// invocations don't interfere.  Stale segments from crashes are cleaned up
// in main() via shm_unlink before the run begins.

#include "../ipc/chi_ipc.hh"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

using namespace gem5::ruby::chi_ipc;

// ─────────────────────────────────────────────────────────────────────────────
// Bookkeeping
// ─────────────────────────────────────────────────────────────────────────────

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { printf("  PASS: %s\n", msg); g_pass++; } \
        else      { printf("  FAIL: %s  (line %d)\n", msg, __LINE__); g_fail++; } \
    } while(0)

static std::string shm_name(int n)
{
    static const pid_t pid = getpid();
    return std::string("/chi_ipc_test_") + std::to_string(pid)
           + "_" + std::to_string(n);
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. Lifecycle: create, attach, destroy
// ─────────────────────────────────────────────────────────────────────────────

static void test_lifecycle()
{
    printf("\n── lifecycle ──────────────────────────────────────────────────\n");
    std::string name = shm_name(1);

    // Creator side
    {
        ChiShmHandle creator(name, /*create=*/true);
        CHECK(creator.valid(), "creator is valid after construction");
        CHECK(creator.layout() != nullptr, "creator layout ptr non-null");

        ChiShmLayout* lay = creator.layout();
        CHECK(lay->gem5_grant_ps.load() == 0,   "gem5_grant_ps starts 0");
        CHECK(lay->questa_grant_ps.load() == 0,  "questa_grant_ps starts 0");
        CHECK(lay->q2g_req.empty(), "q2g_req empty on init");
        CHECK(lay->q2g_rsp.empty(), "q2g_rsp empty on init");
        CHECK(lay->q2g_dat.empty(), "q2g_dat empty on init");
        CHECK(lay->g2q_rsp.empty(), "g2q_rsp empty on init");
        CHECK(lay->g2q_dat.empty(), "g2q_dat empty on init");
        CHECK(lay->g2q_snp.empty(), "g2q_snp empty on init");

        // Attacher side (create=false, then tryOpen)
        ChiShmHandle attacher(name, /*create=*/false);
        CHECK(!attacher.valid(), "attacher not valid before tryOpen");
        bool ok = attacher.tryOpen();
        CHECK(ok, "tryOpen succeeds while creator alive");
        CHECK(attacher.valid(), "attacher valid after tryOpen");
        CHECK(attacher.layout() != nullptr, "attacher layout ptr non-null");

        // Both sides see the same memory
        lay->gem5_grant_ps.store(42000);
        CHECK(attacher.layout()->gem5_grant_ps.load() == 42000,
              "attacher sees write by creator");
    }
    // Creator destroyed → shm_unlink called → tryOpen now fails
    {
        ChiShmHandle late(name, /*create=*/false);
        bool ok = late.tryOpen();
        CHECK(!ok, "tryOpen fails after creator destroyed");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. Ring buffer: basic push / pop
// ─────────────────────────────────────────────────────────────────────────────

static void test_ringbuf_basic()
{
    printf("\n── ring_buf_basic ─────────────────────────────────────────────\n");
    std::string name = shm_name(2);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    // q2g_req: capacity 64
    ChiIpcReq req{};
    req.addr    = 0xDEADBEEF000ULL;
    req.opcode  = 0x01; // ReadShared
    req.txn_id  = 7;
    req.src_id  = 5;
    req.tgt_id  = 3;

    CHECK(lay->q2g_req.empty(), "q2g_req empty initially");

    bool pushed = lay->q2g_req.push(req);
    CHECK(pushed, "push returns true on non-full buffer");
    CHECK(!lay->q2g_req.empty(), "q2g_req not empty after push");

    ChiIpcReq out{};
    bool popped = lay->q2g_req.pop(out);
    CHECK(popped, "pop returns true on non-empty buffer");
    CHECK(lay->q2g_req.empty(), "q2g_req empty after pop");
    CHECK(out.addr    == req.addr,   "popped addr matches");
    CHECK(out.opcode  == req.opcode, "popped opcode matches");
    CHECK(out.txn_id  == req.txn_id, "popped txn_id matches");
    CHECK(out.src_id  == req.src_id, "popped src_id matches");
    CHECK(out.tgt_id  == req.tgt_id, "popped tgt_id matches");

    ChiIpcReq dummy{};
    bool empty_pop = lay->q2g_req.pop(dummy);
    CHECK(!empty_pop, "pop on empty returns false");
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. Ring buffer: full back-pressure
// ─────────────────────────────────────────────────────────────────────────────

static void test_ringbuf_full()
{
    printf("\n── ring_buf_full ──────────────────────────────────────────────\n");
    std::string name = shm_name(3);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    // g2q_snp: capacity 64
    ChiIpcSnp snp{};
    snp.opcode = 0x01;
    snp.addr   = 0x1000;

    int pushed_count = 0;
    for (int i = 0; i < 70; i++) {
        snp.txn_id = static_cast<uint8_t>(i);
        if (lay->g2q_snp.push(snp)) pushed_count++;
    }
    CHECK(pushed_count == 64, "exactly 64 pushes succeed (capacity 64)");

    // One more push must fail
    snp.txn_id = 99;
    bool extra = lay->g2q_snp.push(snp);
    CHECK(!extra, "push on full ring returns false");

    // Drain and verify count
    int pop_count = 0;
    ChiIpcSnp tmp{};
    while (lay->g2q_snp.pop(tmp)) pop_count++;
    CHECK(pop_count == 64, "pop count equals push count");
    CHECK(lay->g2q_snp.empty(), "ring empty after draining");
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. Ring buffer: wrap-around at index boundary
// ─────────────────────────────────────────────────────────────────────────────

static void test_ringbuf_wraparound()
{
    printf("\n── ring_buf_wraparound ────────────────────────────────────────\n");
    std::string name = shm_name(4);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    // Fill and drain repeatedly so head/tail advance past N=64 boundary
    ChiIpcRsp rsp{};
    rsp.opcode = 0x04; // Comp

    // Push 60 items, pop 60, leaving head=60,tail=60
    for (int i = 0; i < 60; i++) {
        rsp.txn_id = static_cast<uint8_t>(i);
        lay->g2q_rsp.push(rsp);
    }
    ChiIpcRsp tmp{};
    for (int i = 0; i < 60; i++) lay->g2q_rsp.pop(tmp);

    // Push 10 more — head wraps: slots 60..63 then 0..5
    for (int i = 0; i < 10; i++) {
        rsp.txn_id = static_cast<uint8_t>(100 + i);
        lay->g2q_rsp.push(rsp);
    }
    int popped = 0;
    uint8_t first_id = 0xFF, last_id = 0xFF;
    while (lay->g2q_rsp.pop(tmp)) {
        if (popped == 0) first_id = tmp.txn_id;
        last_id = tmp.txn_id;
        popped++;
    }
    CHECK(popped == 10, "wrap-around: pop count correct");
    CHECK(first_id == 100, "wrap-around: first txn_id correct (100)");
    CHECK(last_id  == 109, "wrap-around: last txn_id correct (109)");
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. Advisory time markers
// ─────────────────────────────────────────────────────────────────────────────

static void test_time_markers()
{
    printf("\n── time_markers ───────────────────────────────────────────────\n");
    std::string name = shm_name(5);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    lay->gem5_grant_ps.store(1'000'000, std::memory_order_release);
    lay->questa_grant_ps.store(999'000, std::memory_order_release);

    CHECK(lay->gem5_grant_ps.load(std::memory_order_acquire) == 1'000'000,
          "gem5_grant_ps round-trips correctly");
    CHECK(lay->questa_grant_ps.load(std::memory_order_acquire) == 999'000,
          "questa_grant_ps round-trips correctly");

    // Monotonic increment
    uint64_t prev = lay->gem5_grant_ps.load();
    for (int i = 0; i < 5; i++) {
        lay->gem5_grant_ps.store(prev + (i + 1) * 1000);
    }
    CHECK(lay->gem5_grant_ps.load() == prev + 5000,
          "gem5_grant_ps final value after increments");
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. Round-trip: REQ message (q2g direction, producer→consumer via threads)
// ─────────────────────────────────────────────────────────────────────────────

static void test_roundtrip_req()
{
    printf("\n── roundtrip_req ──────────────────────────────────────────────\n");
    std::string name = shm_name(6);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    const int N = 50;
    std::atomic<int> consumed{0};

    // Producer thread (simulates Questa side pushing REQ flits)
    std::thread producer([&] {
        ChiIpcReq req{};
        req.opcode   = 0x01; // ReadShared
        req.src_id   = 10;
        req.tgt_id   = 20;
        req.allow_retry = 1;
        for (int i = 0; i < N; i++) {
            req.txn_id = static_cast<uint8_t>(i & 0xFF);
            req.addr   = 0x1000 + static_cast<uint64_t>(i) * 64;
            while (!lay->q2g_req.push(req))
                std::this_thread::yield();
        }
    });

    // Consumer thread (simulates gem5 draining q2g_req)
    std::thread consumer([&] {
        ChiIpcReq out{};
        int expected = 0;
        while (expected < N) {
            if (lay->q2g_req.pop(out)) {
                // Verify fields on every other item to keep output manageable
                if (expected == 0) {
                    CHECK(out.txn_id == 0,    "rt_req: first txn_id=0");
                    CHECK(out.addr   == 0x1000, "rt_req: first addr correct");
                    CHECK(out.opcode == 0x01,  "rt_req: opcode preserved");
                    CHECK(out.src_id == 10,    "rt_req: src_id preserved");
                    CHECK(out.allow_retry == 1, "rt_req: allow_retry preserved");
                }
                expected++;
                consumed.store(expected);
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    CHECK(consumed.load() == N, "roundtrip_req: all N messages consumed");
    CHECK(lay->q2g_req.empty(), "roundtrip_req: ring empty after test");
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. Round-trip: RSP message (g2q direction)
// ─────────────────────────────────────────────────────────────────────────────

static void test_roundtrip_rsp()
{
    printf("\n── roundtrip_rsp ──────────────────────────────────────────────\n");
    std::string name = shm_name(7);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    const int N = 40;
    std::atomic<int> consumed{0};

    std::thread producer([&] {
        ChiIpcRsp rsp{};
        rsp.opcode   = 0x04; // Comp
        rsp.resp     = 0x01; // UC
        rsp.resp_err = 0x00; // OK
        rsp.src_id   = 20;
        rsp.tgt_id   = 10;
        for (int i = 0; i < N; i++) {
            rsp.txn_id = static_cast<uint8_t>(i);
            rsp.db_id  = static_cast<uint16_t>(i + 100);
            while (!lay->g2q_rsp.push(rsp))
                std::this_thread::yield();
        }
    });

    std::thread consumer([&] {
        ChiIpcRsp out{};
        int expected = 0;
        while (expected < N) {
            if (lay->g2q_rsp.pop(out)) {
                if (expected == 0) {
                    CHECK(out.opcode   == 0x04, "rt_rsp: opcode Comp");
                    CHECK(out.resp     == 0x01, "rt_rsp: resp UC");
                    CHECK(out.resp_err == 0x00, "rt_rsp: resp_err OK");
                    CHECK(out.txn_id   == 0,    "rt_rsp: first txn_id=0");
                    CHECK(out.db_id    == 100,  "rt_rsp: first db_id=100");
                }
                expected++;
                consumed.store(expected);
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    CHECK(consumed.load() == N, "roundtrip_rsp: all N messages consumed");
    CHECK(lay->g2q_rsp.empty(), "roundtrip_rsp: ring empty after test");
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. Round-trip: DAT message (data payload preserved)
// ─────────────────────────────────────────────────────────────────────────────

static void test_roundtrip_dat()
{
    printf("\n── roundtrip_dat ──────────────────────────────────────────────\n");
    std::string name = shm_name(8);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    // Build a DAT message with known data pattern
    ChiIpcDat dat{};
    dat.opcode    = 0x04; // CompData
    dat.txn_id    = 42;
    dat.src_id    = 20;
    dat.tgt_id    = 10;
    dat.home_n_id = 20;
    dat.resp      = 0x02; // SC
    dat.data_len  = 64;
    dat.data_id   = 0;
    for (int i = 0; i < 64; i++) {
        dat.data[i] = static_cast<uint8_t>(i ^ 0xAA);
        dat.be[i]   = 0xFF;
    }

    bool pushed = lay->g2q_dat.push(dat);
    CHECK(pushed, "dat push succeeds");

    ChiIpcDat out{};
    bool popped = lay->g2q_dat.pop(out);
    CHECK(popped, "dat pop succeeds");

    CHECK(out.opcode    == 0x04, "dat: opcode CompData");
    CHECK(out.txn_id    == 42,   "dat: txn_id preserved");
    CHECK(out.home_n_id == 20,   "dat: home_n_id preserved");
    CHECK(out.resp      == 0x02, "dat: resp SC preserved");
    CHECK(out.data_len  == 64,   "dat: data_len preserved");
    CHECK(out.data_id   == 0,    "dat: data_id preserved");

    bool data_ok = true;
    bool be_ok   = true;
    for (int i = 0; i < 64; i++) {
        if (out.data[i] != static_cast<uint8_t>(i ^ 0xAA)) data_ok = false;
        if (out.be[i]   != 0xFF)                            be_ok   = false;
    }
    CHECK(data_ok, "dat: all 64 data bytes preserved");
    CHECK(be_ok,   "dat: all 64 byte-enable bytes preserved");
}

// ─────────────────────────────────────────────────────────────────────────────
// 9. Round-trip: SNP message (g2q direction)
// ─────────────────────────────────────────────────────────────────────────────

static void test_roundtrip_snp()
{
    printf("\n── roundtrip_snp ──────────────────────────────────────────────\n");
    std::string name = shm_name(9);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    ChiIpcSnp snp{};
    snp.addr            = 0xCAFE0000000ULL;
    snp.opcode          = 0x01; // SnpShared
    snp.txn_id          = 77;
    snp.src_id          = 5;
    snp.fwd_n_id        = 3;
    snp.fwd_txn_id      = 12;
    snp.ret_to_src      = 1;
    snp.do_not_goto_sd  = 0;
    snp.do_not_data_pull = 0;
    snp.qos             = 7;
    snp.ns              = 1;

    lay->g2q_snp.push(snp);

    ChiIpcSnp out{};
    lay->g2q_snp.pop(out);

    CHECK(out.addr            == snp.addr,            "snp: addr preserved");
    CHECK(out.opcode          == snp.opcode,          "snp: opcode preserved");
    CHECK(out.txn_id          == snp.txn_id,          "snp: txn_id preserved");
    CHECK(out.src_id          == snp.src_id,          "snp: src_id preserved");
    CHECK(out.fwd_n_id        == snp.fwd_n_id,        "snp: fwd_n_id preserved");
    CHECK(out.fwd_txn_id      == snp.fwd_txn_id,      "snp: fwd_txn_id preserved");
    CHECK(out.ret_to_src      == snp.ret_to_src,      "snp: ret_to_src preserved");
    CHECK(out.do_not_goto_sd  == snp.do_not_goto_sd,  "snp: do_not_goto_sd preserved");
    CHECK(out.do_not_data_pull == snp.do_not_data_pull,"snp: do_not_data_pull preserved");
    CHECK(out.qos             == snp.qos,             "snp: qos preserved");
    CHECK(out.ns              == snp.ns,              "snp: ns preserved");
}

// ─────────────────────────────────────────────────────────────────────────────
// 10. tryOpen retry: attacher waits until creator appears
// ─────────────────────────────────────────────────────────────────────────────

static void test_tryopen_retry()
{
    printf("\n── tryopen_retry ──────────────────────────────────────────────\n");
    std::string name = shm_name(10);

    std::atomic<bool> attached{false};
    std::atomic<bool> creator_ready{false};

    // Attacher thread starts trying before creator exists
    std::thread attacher_t([&] {
        ChiShmHandle att(name, /*create=*/false);
        // Try several times before the creator appears
        int tries = 0;
        while (!att.tryOpen()) {
            tries++;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (tries > 200) break;  // 400ms timeout safety
        }
        attached.store(att.valid());
        if (att.valid()) {
            // Write a sentinel so we can verify cross-process visibility
            att.layout()->questa_grant_ps.store(0xDEAD);
        }
    });

    // Creator appears after a small delay
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        ChiShmHandle creator(name, /*create=*/true);
        creator_ready.store(true);
        // Wait for attacher to attach and write the sentinel
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(attached.load(), "tryopen_retry: attacher attached successfully");
        CHECK(creator.layout()->questa_grant_ps.load() == 0xDEAD,
              "tryopen_retry: attacher write visible to creator");
        attacher_t.join();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 11. Multi-buffer concurrent: all 6 ring buffers used simultaneously
// ─────────────────────────────────────────────────────────────────────────────

static void test_concurrent_all_channels()
{
    printf("\n── concurrent_all_channels ────────────────────────────────────\n");
    std::string name = shm_name(11);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    const int N = 30;

    // Six producer threads, one per buffer
    auto push_req = [&] {
        ChiIpcReq r{}; r.opcode = 1;
        for (int i = 0; i < N; i++) { r.txn_id = i; while (!lay->q2g_req.push(r)) std::this_thread::yield(); }
    };
    auto push_qrsp = [&] {
        ChiIpcRsp r{}; r.opcode = 4;
        for (int i = 0; i < N; i++) { r.txn_id = i; while (!lay->q2g_rsp.push(r)) std::this_thread::yield(); }
    };
    auto push_qdat = [&] {
        ChiIpcDat d{}; d.opcode = 4; d.data_len = 64;
        for (int i = 0; i < N; i++) { d.txn_id = i; while (!lay->q2g_dat.push(d)) std::this_thread::yield(); }
    };
    auto push_grsp = [&] {
        ChiIpcRsp r{}; r.opcode = 4;
        for (int i = 0; i < N; i++) { r.txn_id = i; while (!lay->g2q_rsp.push(r)) std::this_thread::yield(); }
    };
    auto push_gdat = [&] {
        ChiIpcDat d{}; d.opcode = 4; d.data_len = 64;
        for (int i = 0; i < N; i++) { d.txn_id = i; while (!lay->g2q_dat.push(d)) std::this_thread::yield(); }
    };
    auto push_snp = [&] {
        ChiIpcSnp s{}; s.opcode = 1;
        for (int i = 0; i < N; i++) { s.txn_id = i; while (!lay->g2q_snp.push(s)) std::this_thread::yield(); }
    };

    std::thread t1(push_req), t2(push_qrsp), t3(push_qdat),
                t4(push_grsp), t5(push_gdat), t6(push_snp);
    t1.join(); t2.join(); t3.join(); t4.join(); t5.join(); t6.join();

    // Drain and count
    int nreq = 0, nqrsp = 0, nqdat = 0, ngrsp = 0, ngdat = 0, nsnp = 0;
    { ChiIpcReq t; while (lay->q2g_req.pop(t)) nreq++;  }
    { ChiIpcRsp t; while (lay->q2g_rsp.pop(t)) nqrsp++; }
    { ChiIpcDat t; while (lay->q2g_dat.pop(t)) nqdat++; }
    { ChiIpcRsp t; while (lay->g2q_rsp.pop(t)) ngrsp++; }
    { ChiIpcDat t; while (lay->g2q_dat.pop(t)) ngdat++; }
    { ChiIpcSnp t; while (lay->g2q_snp.pop(t)) nsnp++;  }

    CHECK(nreq  == N, "concurrent: q2g_req count");
    CHECK(nqrsp == N, "concurrent: q2g_rsp count");
    CHECK(nqdat == N, "concurrent: q2g_dat count");
    CHECK(ngrsp == N, "concurrent: g2q_rsp count");
    CHECK(ngdat == N, "concurrent: g2q_dat count");
    CHECK(nsnp  == N, "concurrent: g2q_snp count");
}

// ─────────────────────────────────────────────────────────────────────────────
// 12. Struct size / alignment sanity
// ─────────────────────────────────────────────────────────────────────────────

static void test_struct_sizes()
{
    printf("\n── struct_sizes ───────────────────────────────────────────────\n");
    // These must not change without bumping the protocol version.
    CHECK(sizeof(ChiIpcRsp) == 14,  "ChiIpcRsp is 14 bytes");
    CHECK(sizeof(ChiIpcDat) == 160, "ChiIpcDat is 160 bytes");
    CHECK(sizeof(ChiIpcSnp) == 24,  "ChiIpcSnp is 24 bytes");

    // alignment of the layout's time-sync counters
    ChiShmHandle h(shm_name(12), /*create=*/true);
    ChiShmLayout* lay = h.layout();
    auto offset_gem5  = reinterpret_cast<uintptr_t>(&lay->gem5_grant_ps)
                        - reinterpret_cast<uintptr_t>(lay);
    auto offset_questa = reinterpret_cast<uintptr_t>(&lay->questa_grant_ps)
                         - reinterpret_cast<uintptr_t>(lay);
    CHECK(offset_gem5  % 64 == 0, "gem5_grant_ps is 64-byte aligned");
    CHECK(offset_questa % 64 == 0, "questa_grant_ps is 64-byte aligned");
}

// ─────────────────────────────────────────────────────────────────────────────
// 13. ChiIpcReq — all 14 fields preserved
// ─────────────────────────────────────────────────────────────────────────────

static void test_req_all_fields()
{
    printf("\n── req_all_fields ─────────────────────────────────────────────\n");
    std::string name = shm_name(13);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    ChiIpcReq r{};
    r.addr        = 0xFEDCBA9876543210ULL & ~0x3FULL; // cache-line aligned
    r.opcode      = 0x1B;  // ReadOnce
    r.txn_id      = 0xFF;  // max 8-bit
    r.src_id      = 0x7F;  // max 7-bit NodeID
    r.tgt_id      = 0x55;
    r.qos         = 0xF;   // max 4-bit
    r.ns          = 1;
    r.allow_retry = 1;
    r.order       = 0x3;   // max 2-bit
    r.mem_attr    = 0xA;
    r.size        = 0x6;   // 64 bytes
    r.snoop_me    = 1;
    r.exc_l       = 1;     // Excl/SnoopMe share same flit bit (EXCL_LO == SNOOPME_LO)
    r.pcrd_type   = 0x9;   // max fits 4 bits

    lay->q2g_req.push(r);
    ChiIpcReq out{};
    lay->q2g_req.pop(out);

    CHECK(out.addr        == r.addr,        "req: addr (all bits)");
    CHECK(out.opcode      == r.opcode,      "req: opcode");
    CHECK(out.txn_id      == r.txn_id,      "req: txn_id max (0xFF)");
    CHECK(out.src_id      == r.src_id,      "req: src_id max (0x7F)");
    CHECK(out.tgt_id      == r.tgt_id,      "req: tgt_id");
    CHECK(out.qos         == r.qos,         "req: qos max (0xF)");
    CHECK(out.ns          == r.ns,          "req: ns");
    CHECK(out.allow_retry == r.allow_retry, "req: allow_retry");
    CHECK(out.order       == r.order,       "req: order max (0x3)");
    CHECK(out.mem_attr    == r.mem_attr,    "req: mem_attr");
    CHECK(out.size        == r.size,        "req: size");
    CHECK(out.exc_l       == r.exc_l,       "req: exc_l");
    CHECK(out.snoop_me    == r.snoop_me,    "req: snoop_me");
    CHECK(out.pcrd_type   == r.pcrd_type,   "req: pcrd_type");
}

// ─────────────────────────────────────────────────────────────────────────────
// 14. DAT ring buffers have capacity 32 (not 64)
// ─────────────────────────────────────────────────────────────────────────────

static void test_dat_capacity_32()
{
    printf("\n── dat_capacity_32 ────────────────────────────────────────────\n");
    std::string name = shm_name(14);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    ChiIpcDat d{};
    d.opcode   = 0x04; // CompData
    d.data_len = 64;

    // q2g_dat: capacity 32
    int n_q2g = 0;
    for (int i = 0; i < 40; i++) {
        d.txn_id = static_cast<uint8_t>(i);
        if (lay->q2g_dat.push(d)) n_q2g++;
    }
    CHECK(n_q2g == 32, "q2g_dat: capacity exactly 32");

    // g2q_dat: capacity 32
    int n_g2q = 0;
    for (int i = 0; i < 40; i++) {
        d.txn_id = static_cast<uint8_t>(i + 100);
        if (lay->g2q_dat.push(d)) n_g2q++;
    }
    CHECK(n_g2q == 32, "g2q_dat: capacity exactly 32");

    // Extra push must fail
    bool extra = lay->q2g_dat.push(d);
    CHECK(!extra, "q2g_dat: push-on-full returns false");

    // Drain both
    int drained = 0;
    ChiIpcDat tmp{};
    while (lay->q2g_dat.pop(tmp)) drained++;
    while (lay->g2q_dat.pop(tmp)) drained++;
    CHECK(drained == 64, "dat rings: drained exactly 64 total");
}

// ─────────────────────────────────────────────────────────────────────────────
// 15. DAT multi-beat: two beats with data_id=0 and data_id=1
// ─────────────────────────────────────────────────────────────────────────────

static void test_dat_multibeat()
{
    printf("\n── dat_multibeat ──────────────────────────────────────────────\n");
    std::string name = shm_name(15);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    // Beat 0: first half of 128-byte line
    ChiIpcDat beat0{};
    beat0.opcode   = 0x04; // CompData
    beat0.txn_id   = 42;
    beat0.data_id  = 0;
    beat0.data_len = 64;
    for (int i = 0; i < 64; i++) beat0.data[i] = static_cast<uint8_t>(i);
    for (int i = 0; i < 64; i++) beat0.be[i]   = 0xFF;

    // Beat 1: second half
    ChiIpcDat beat1{};
    beat1.opcode   = 0x04;
    beat1.txn_id   = 42;
    beat1.data_id  = 1;
    beat1.data_len = 64;
    for (int i = 0; i < 64; i++) beat1.data[i] = static_cast<uint8_t>(i + 64);
    for (int i = 0; i < 64; i++) beat1.be[i]   = 0xFF;

    lay->g2q_dat.push(beat0);
    lay->g2q_dat.push(beat1);

    ChiIpcDat out0{}, out1{};
    lay->g2q_dat.pop(out0);
    lay->g2q_dat.pop(out1);

    CHECK(out0.data_id == 0, "multibeat: beat0 data_id=0");
    CHECK(out1.data_id == 1, "multibeat: beat1 data_id=1");
    CHECK(out0.txn_id  == 42, "multibeat: both beats same txn_id");
    CHECK(out1.txn_id  == 42, "multibeat: beat1 txn_id");

    bool data0_ok = true, data1_ok = true;
    for (int i = 0; i < 64; i++) {
        if (out0.data[i] != static_cast<uint8_t>(i))       data0_ok = false;
        if (out1.data[i] != static_cast<uint8_t>(i + 64))  data1_ok = false;
    }
    CHECK(data0_ok, "multibeat: beat0 data pattern correct");
    CHECK(data1_ok, "multibeat: beat1 data pattern correct");
}

// ─────────────────────────────────────────────────────────────────────────────
// 16. DAT with data_len=0 round-trips without crash
// ─────────────────────────────────────────────────────────────────────────────

static void test_dat_zero_len()
{
    printf("\n── dat_zero_len ───────────────────────────────────────────────\n");
    std::string name = shm_name(16);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    ChiIpcDat d{};
    d.opcode   = 0x04;
    d.txn_id   = 7;
    d.data_len = 0;   // zero data length (no-data completion marker)
    // data[] left zero-initialised

    bool pushed = lay->q2g_dat.push(d);
    CHECK(pushed, "dat zero_len: push succeeds");

    ChiIpcDat out{};
    bool popped = lay->q2g_dat.pop(out);
    CHECK(popped, "dat zero_len: pop succeeds");
    CHECK(out.data_len == 0,  "dat zero_len: data_len=0 preserved");
    CHECK(out.txn_id   == 7,  "dat zero_len: txn_id preserved");
    CHECK(out.opcode   == 0x04, "dat zero_len: opcode preserved");

    // data bytes should still be zero
    bool all_zero = true;
    for (int i = 0; i < 64; i++) if (out.data[i] != 0) all_zero = false;
    CHECK(all_zero, "dat zero_len: data bytes are zero");
}

// ─────────────────────────────────────────────────────────────────────────────
// 17. SNP: all combinations of three bool flags
// ─────────────────────────────────────────────────────────────────────────────

static void test_snp_flag_combinations()
{
    printf("\n── snp_flag_combinations ──────────────────────────────────────\n");
    std::string name = shm_name(17);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    // Push 4 SNPs covering all {ret_to_src, do_not_goto_sd, do_not_data_pull}
    // combinations (3 bits → 8, but we test the most meaningful 4)
    struct FlagSet { uint8_t rts, dngs, dndp; };
    FlagSet combos[4] = {
        {0, 0, 0},
        {1, 0, 0},
        {0, 1, 1},
        {1, 1, 1},
    };

    for (auto& c : combos) {
        ChiIpcSnp s{};
        s.opcode           = 0x01;
        s.txn_id           = 10;
        s.ret_to_src       = c.rts;
        s.do_not_goto_sd   = c.dngs;
        s.do_not_data_pull = c.dndp;
        lay->g2q_snp.push(s);
    }

    for (auto& c : combos) {
        ChiIpcSnp out{};
        lay->g2q_snp.pop(out);
        CHECK(out.ret_to_src       == c.rts,  "snp flags: ret_to_src preserved");
        CHECK(out.do_not_goto_sd   == c.dngs, "snp flags: do_not_goto_sd preserved");
        CHECK(out.do_not_data_pull == c.dndp, "snp flags: do_not_data_pull preserved");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 18. Two shm segments are fully independent (no cross-talk)
// ─────────────────────────────────────────────────────────────────────────────

static void test_two_segments_independent()
{
    printf("\n── two_segments_independent ───────────────────────────────────\n");
    std::string nameA = shm_name(181);
    std::string nameB = shm_name(182);

    ChiShmHandle hA(nameA, /*create=*/true);
    ChiShmHandle hB(nameB, /*create=*/true);

    ChiIpcReq rA{}, rB{};
    rA.txn_id = 0xAA;  rA.src_id = 1;
    rB.txn_id = 0xBB;  rB.src_id = 2;

    hA.layout()->q2g_req.push(rA);
    hB.layout()->q2g_req.push(rB);

    // Segment A should only see rA, not rB
    ChiIpcReq outA{};
    hA.layout()->q2g_req.pop(outA);
    CHECK(outA.txn_id == 0xAA, "seg_A: reads its own txn_id");
    CHECK(hA.layout()->q2g_req.empty(), "seg_A: empty after pop");

    // Segment B should only see rB
    ChiIpcReq outB{};
    hB.layout()->q2g_req.pop(outB);
    CHECK(outB.txn_id == 0xBB, "seg_B: reads its own txn_id");
    CHECK(hB.layout()->q2g_req.empty(), "seg_B: empty after pop");

    // Writes to B's time marker don't affect A
    hB.layout()->gem5_grant_ps.store(999'999);
    CHECK(hA.layout()->gem5_grant_ps.load() == 0, "seg_A: gem5_grant_ps unaffected by seg_B write");
}

// ─────────────────────────────────────────────────────────────────────────────
// 19. Ring capacity off-by-one: fill to N-1, pop 1, push 2 (should succeed)
// ─────────────────────────────────────────────────────────────────────────────

static void test_ring_boundary_n_minus_1()
{
    printf("\n── ring_boundary_n_minus_1 ────────────────────────────────────\n");
    std::string name = shm_name(19);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    // q2g_req: N=64. Fill 63 items (N-1).
    ChiIpcReq r{}; r.opcode = 1;
    for (int i = 0; i < 63; i++) {
        r.txn_id = static_cast<uint8_t>(i);
        lay->q2g_req.push(r);
    }

    // Push one more → reaches exactly N; ring is now full
    r.txn_id = 63;
    bool full_push = lay->q2g_req.push(r);
    CHECK(full_push, "boundary: 64th push (full) succeeds");

    // Next push must fail
    r.txn_id = 99;
    bool over_push = lay->q2g_req.push(r);
    CHECK(!over_push, "boundary: 65th push (over-full) fails");

    // Pop one; now ring has 63 items; exactly one slot free
    ChiIpcReq tmp{};
    lay->q2g_req.pop(tmp);
    CHECK(tmp.txn_id == 0, "boundary: first pop gives txn_id=0");

    // Now push two; first succeeds (fills the freed slot), second fails
    r.txn_id = 100;
    bool p1 = lay->q2g_req.push(r);
    CHECK(p1, "boundary: push after pop succeeds (refills slot)");

    r.txn_id = 101;
    bool p2 = lay->q2g_req.push(r);
    CHECK(!p2, "boundary: second push after pop fails (full again)");
}

// ─────────────────────────────────────────────────────────────────────────────
// 20. Head/tail are monotonically increasing across many fill+drain cycles
// ─────────────────────────────────────────────────────────────────────────────

static void test_head_tail_monotonic()
{
    printf("\n── head_tail_monotonic ────────────────────────────────────────\n");
    std::string name = shm_name(20);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    // 3 full fill+drain cycles on g2q_rsp (N=64)
    ChiIpcRsp r{}; r.opcode = 4;
    ChiIpcRsp tmp{};
    for (int cycle = 0; cycle < 3; cycle++) {
        for (int i = 0; i < 64; i++) {
            r.txn_id = static_cast<uint8_t>(i);
            lay->g2q_rsp.push(r);
        }
        for (int i = 0; i < 64; i++) lay->g2q_rsp.pop(tmp);
    }

    // After 3×64 = 192 pushes+pops, head and tail should both be 192
    uint32_t head = lay->g2q_rsp.head.load(std::memory_order_acquire);
    uint32_t tail = lay->g2q_rsp.tail.load(std::memory_order_acquire);
    CHECK(head == 192, "monotonic: head == 192 after 3 cycles");
    CHECK(tail == 192, "monotonic: tail == 192 after 3 cycles");
    CHECK(lay->g2q_rsp.empty(), "monotonic: ring empty at end");
}

// ─────────────────────────────────────────────────────────────────────────────
// 21. shm_name() accessor returns the name given at construction
// ─────────────────────────────────────────────────────────────────────────────

static void test_shm_name_accessor()
{
    printf("\n── shm_name_accessor ──────────────────────────────────────────\n");
    std::string expected = shm_name(21);
    ChiShmHandle h(expected, /*create=*/true);
    CHECK(h.name() == expected, "shm_name: name() matches constructor argument");

    // Attacher side also retains the name
    ChiShmHandle att(expected, /*create=*/false);
    att.tryOpen();
    CHECK(att.name() == expected, "shm_name: attacher name() matches");
}

// ─────────────────────────────────────────────────────────────────────────────
// 22. High-volume threaded stress: 1 000 REQs through q2g_req
// ─────────────────────────────────────────────────────────────────────────────

static void test_stress_high_volume()
{
    printf("\n── stress_high_volume ─────────────────────────────────────────\n");
    std::string name = shm_name(22);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    const int N = 1000;
    std::atomic<int> total_consumed{0};
    std::atomic<bool> producer_done{false};

    std::thread producer([&] {
        ChiIpcReq r{};
        r.opcode = 0x01;
        for (int i = 0; i < N; i++) {
            r.txn_id = static_cast<uint8_t>(i & 0xFF);
            r.addr   = static_cast<uint64_t>(i) * 64;
            while (!lay->q2g_req.push(r)) std::this_thread::yield();
        }
        producer_done.store(true);
    });

    std::thread consumer([&] {
        ChiIpcReq out{};
        int count = 0;
        while (count < N) {
            if (lay->q2g_req.pop(out)) count++;
            else std::this_thread::yield();
        }
        total_consumed.store(count);
    });

    producer.join();
    consumer.join();

    CHECK(total_consumed.load() == N, "stress: all 1000 REQs consumed");
    CHECK(lay->q2g_req.empty(),       "stress: ring empty after stress run");
    CHECK(producer_done.load(),        "stress: producer completed cleanly");
}

// ─────────────────────────────────────────────────────────────────────────────
// 23. RSP full field coverage (db_id, pcrd_type, resp_err edge values)
// ─────────────────────────────────────────────────────────────────────────────

static void test_rsp_full_fields()
{
    printf("\n── rsp_full_fields ────────────────────────────────────────────\n");
    std::string name = shm_name(23);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    ChiIpcRsp r{};
    r.opcode    = 0x0B;  // CompDBIDResp
    r.txn_id    = 0xFE;
    r.src_id    = 0x3F;
    r.tgt_id    = 0x01;
    r.qos       = 0xE;
    r.resp      = 0x06;  // SC_Dirty
    r.resp_err  = 0x02;  // EXOK
    r.pcrd_type = 0xF;   // max 4-bit
    r.db_id     = 0x00FF; // max 8-bit TxnID width (TXNID_W=8 → max 255)

    lay->g2q_rsp.push(r);
    ChiIpcRsp out{};
    lay->g2q_rsp.pop(out);

    CHECK(out.opcode    == r.opcode,    "rsp_full: opcode");
    CHECK(out.txn_id    == r.txn_id,    "rsp_full: txn_id (0xFE)");
    CHECK(out.src_id    == r.src_id,    "rsp_full: src_id");
    CHECK(out.tgt_id    == r.tgt_id,    "rsp_full: tgt_id");
    CHECK(out.qos       == r.qos,       "rsp_full: qos max (0xE)");
    CHECK(out.resp      == r.resp,      "rsp_full: resp SC_Dirty");
    CHECK(out.resp_err  == r.resp_err,  "rsp_full: resp_err EXOK");
    CHECK(out.pcrd_type == r.pcrd_type, "rsp_full: pcrd_type max (0xF)");
    CHECK(out.db_id     == r.db_id,     "rsp_full: db_id (0xFF)");
}

// ─────────────────────────────────────────────────────────────────────────────
// 24. Interleaved push/pop (not fill-then-drain) on a single ring
// ─────────────────────────────────────────────────────────────────────────────

static void test_interleaved_push_pop()
{
    printf("\n── interleaved_push_pop ───────────────────────────────────────\n");
    std::string name = shm_name(24);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    // Push items with a global monotonically increasing push_seq; pop items
    // and verify they come out in that same global order (strict FIFO).
    ChiIpcRsp r{};  r.opcode = 4;
    ChiIpcRsp out{};

    std::vector<uint8_t> push_order;  // track push sequence globally
    bool order_ok = true;
    int  pop_idx  = 0;

    // Pattern: push 2, pop 1, push 1, pop 2 — repeat 30 times
    for (int round = 0; round < 30; round++) {
        // push 2
        for (int k = 0; k < 2; k++) {
            r.txn_id = static_cast<uint8_t>(push_order.size() & 0xFF);
            push_order.push_back(r.txn_id);
            lay->g2q_rsp.push(r);
        }
        // pop 1
        lay->g2q_rsp.pop(out);
        if (out.txn_id != push_order[pop_idx]) order_ok = false;
        pop_idx++;

        // push 1
        r.txn_id = static_cast<uint8_t>(push_order.size() & 0xFF);
        push_order.push_back(r.txn_id);
        lay->g2q_rsp.push(r);

        // pop 2
        for (int k = 0; k < 2; k++) {
            lay->g2q_rsp.pop(out);
            if (out.txn_id != push_order[pop_idx]) order_ok = false;
            pop_idx++;
        }
    }

    // Drain remaining (ring depth after 30 rounds: +3 pushes, -3 pops = 0)
    int leftover = 0;
    while (lay->g2q_rsp.pop(out)) leftover++;

    CHECK(order_ok,        "interleaved: FIFO order maintained across mixed push/pop");
    CHECK(leftover == 0,   "interleaved: ring empty after balanced push/pop");
    CHECK(pop_idx == static_cast<int>(push_order.size()),
                           "interleaved: all pushed items were consumed");
}

// ─────────────────────────────────────────────────────────────────────────────
// 25. Binary data pattern: alternating 0x00 / 0xFF bytes round-trip
// ─────────────────────────────────────────────────────────────────────────────

static void test_dat_binary_pattern()
{
    printf("\n── dat_binary_pattern ─────────────────────────────────────────\n");
    std::string name = shm_name(25);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    ChiIpcDat d{};
    d.opcode   = 0x04;
    d.txn_id   = 55;
    d.data_len = 64;

    // Pattern: even bytes = 0x00, odd bytes = 0xFF
    for (int i = 0; i < 64; i++) {
        d.data[i] = (i & 1) ? 0xFF : 0x00;
        d.be[i]   = (i & 1) ? 0xFF : 0x00;
    }

    lay->q2g_dat.push(d);
    ChiIpcDat out{};
    lay->q2g_dat.pop(out);

    bool data_ok = true, be_ok = true;
    for (int i = 0; i < 64; i++) {
        uint8_t expect_data = (i & 1) ? 0xFF : 0x00;
        uint8_t expect_be   = (i & 1) ? 0xFF : 0x00;
        if (out.data[i] != expect_data) data_ok = false;
        if (out.be[i]   != expect_be)   be_ok   = false;
    }
    CHECK(data_ok, "binary_pattern: alternating 0x00/0xFF data bytes");
    CHECK(be_ok,   "binary_pattern: alternating 0x00/0xFF BE bytes");
}

// ─────────────────────────────────────────────────────────────────────────────
// 26. layout pointer is at the start of the mmap'd region (offset 0)
// ─────────────────────────────────────────────────────────────────────────────

static void test_layout_offset_zero()
{
    printf("\n── layout_offset_zero ─────────────────────────────────────────\n");
    std::string name = shm_name(26);
    ChiShmHandle creator(name, /*create=*/true);

    // Independently open the same segment and mmap it via shm_open
    int fd = shm_open(name.c_str(), O_RDONLY, 0);
    CHECK(fd >= 0, "layout_offset: shm_open for verification succeeds");

    if (fd >= 0) {
        void* p = mmap(nullptr, sizeof(ChiShmLayout),
                       PROT_READ, MAP_SHARED, fd, 0);
        CHECK(p != MAP_FAILED, "layout_offset: mmap for verification succeeds");
        if (p != MAP_FAILED) {
            // The creator placed a ChiShmLayout at the very start of the file.
            // The two pointers should map to the same physical page, so writing
            // via the creator's pointer and reading via ours gives the same value.
            creator.layout()->gem5_grant_ps.store(0xC0FFEE);
            std::atomic<uint64_t>* their_ps =
                reinterpret_cast<std::atomic<uint64_t>*>(p);
            uint64_t val = their_ps->load(std::memory_order_acquire);
            CHECK(val == 0xC0FFEE, "layout_offset: layout starts at mmap offset 0");
            munmap(p, sizeof(ChiShmLayout));
        }
        close(fd);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 27. ChiIpcReq.exp_comp_ack roundtrips
// ─────────────────────────────────────────────────────────────────────────────

static void test_req_exp_comp_ack()
{
    printf("\n── req_exp_comp_ack ───────────────────────────────────────────\n");
    std::string name = shm_name(271);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    // exp_comp_ack = 1
    ChiIpcReq r1{};
    r1.opcode       = 0x01; // ReadShared
    r1.exp_comp_ack = 1;
    lay->q2g_req.push(r1);
    ChiIpcReq o1{};
    lay->q2g_req.pop(o1);
    CHECK(o1.exp_comp_ack == 1, "req_exp_comp_ack: value=1 preserved");

    // exp_comp_ack = 0
    ChiIpcReq r0{};
    r0.opcode       = 0x30; // ReadNoSnp
    r0.exp_comp_ack = 0;
    lay->q2g_req.push(r0);
    ChiIpcReq o0{};
    lay->q2g_req.pop(o0);
    CHECK(o0.exp_comp_ack == 0, "req_exp_comp_ack: value=0 preserved");

    // Check exp_comp_ack is distinct from adjacent fields (pcrd_type)
    ChiIpcReq r2{};
    r2.pcrd_type    = 0xF;
    r2.exp_comp_ack = 1;
    lay->q2g_req.push(r2);
    ChiIpcReq o2{};
    lay->q2g_req.pop(o2);
    CHECK(o2.pcrd_type    == 0xF, "req_exp_comp_ack: pcrd_type not clobbered");
    CHECK(o2.exp_comp_ack == 1,   "req_exp_comp_ack: exp_comp_ack=1 with pcrd_type=0xF");
}

// ─────────────────────────────────────────────────────────────────────────────
// 28. ChiIpcRsp.fwd_state and data_pull roundtrip
// ─────────────────────────────────────────────────────────────────────────────

static void test_rsp_fwd_state_data_pull()
{
    printf("\n── rsp_fwd_state_data_pull ────────────────────────────────────\n");
    std::string name = shm_name(281);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    // All three fwd_state bits + data_pull combinations
    for (uint8_t fs = 0; fs < 8; fs++) {
        for (uint8_t dp = 0; dp < 2; dp++) {
            ChiIpcRsp r{};
            r.opcode     = 0x04;
            r.txn_id     = static_cast<uint8_t>(fs * 2 + dp);
            r.fwd_state  = fs;
            r.data_pull  = dp;
            lay->g2q_rsp.push(r);
            ChiIpcRsp out{};
            lay->g2q_rsp.pop(out);
            CHECK(out.fwd_state == fs,
                  ("rsp fwd_state=" + std::to_string(fs)).c_str());
            CHECK(out.data_pull == dp,
                  ("rsp data_pull=" + std::to_string(dp) + " (fwd_state=" +
                   std::to_string(fs) + ")").c_str());
        }
    }

    // Verify neither field clobbers the others
    ChiIpcRsp r{};
    r.opcode     = 0x0B; // CompDBIDResp
    r.txn_id     = 0x77;
    r.resp       = 0x02;
    r.resp_err   = 0x01;
    r.db_id      = 0x55;
    r.fwd_state  = 0x05;
    r.data_pull  = 0x01;
    lay->g2q_rsp.push(r);
    ChiIpcRsp out{};
    lay->g2q_rsp.pop(out);
    CHECK(out.resp      == 0x02, "rsp: resp not clobbered by fwd_state");
    CHECK(out.resp_err  == 0x01, "rsp: resp_err not clobbered by data_pull");
    CHECK(out.db_id     == 0x55, "rsp: db_id not clobbered by new fields");
    CHECK(out.fwd_state == 0x05, "rsp: fwd_state=5 preserved alongside other fields");
    CHECK(out.data_pull == 0x01, "rsp: data_pull=1 preserved alongside other fields");
}

// ─────────────────────────────────────────────────────────────────────────────
// 29. ChiIpcDat.cc_id roundtrip (all 4 values, 2-bit field)
// ─────────────────────────────────────────────────────────────────────────────

static void test_dat_cc_id()
{
    printf("\n── dat_cc_id ──────────────────────────────────────────────────\n");
    std::string name = shm_name(291);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    for (uint8_t cc = 0; cc < 4; cc++) {
        ChiIpcDat d{};
        d.opcode   = 0x04;
        d.txn_id   = cc;
        d.cc_id    = cc;
        d.data_len = 64;
        lay->g2q_dat.push(d);
        ChiIpcDat out{};
        lay->g2q_dat.pop(out);
        CHECK(out.cc_id  == cc, ("dat cc_id=" + std::to_string(cc) + " preserved").c_str());
        CHECK(out.txn_id == cc, "dat txn_id not clobbered by cc_id");
    }

    // cc_id must not affect data[] or be[]
    ChiIpcDat d{};
    d.opcode   = 0x04;
    d.cc_id    = 0x03;  // max 2-bit
    d.data_len = 64;
    for (int i = 0; i < 64; i++) { d.data[i] = 0xCD; d.be[i] = 0xFF; }
    lay->g2q_dat.push(d);
    ChiIpcDat out{};
    lay->g2q_dat.pop(out);
    CHECK(out.cc_id == 0x03, "dat cc_id=3 (max 2-bit) preserved");
    bool data_ok = true;
    for (int i = 0; i < 64; i++) if (out.data[i] != 0xCD) data_ok = false;
    CHECK(data_ok, "dat data[] not clobbered by cc_id=3");
}

// ─────────────────────────────────────────────────────────────────────────────
// (renumbered) ChiIpcReq struct size: 32 bytes, no hidden trailing padding
// ─────────────────────────────────────────────────────────────────────────────

static void test_req_struct_size()
{
    printf("\n── req_struct_size ────────────────────────────────────────────\n");
    // sizeof must be exactly 32 — _pad[9] makes all padding explicit.
    // Previously _pad[3] left 6 hidden bytes; this test catches any regression.
    CHECK(sizeof(ChiIpcReq) == 32, "ChiIpcReq is exactly 32 bytes");

    // Verify _pad bytes zero-initialised when using {} init (no garbage leakage)
    ChiIpcReq r{};
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(&r);
    bool tail_zero = true;
    for (size_t i = 23; i < 32; i++) if (raw[i] != 0) tail_zero = false;
    CHECK(tail_zero, "ChiIpcReq _pad bytes are zero after {} init");

    // Alignment: struct must be 8-byte aligned (due to uint64_t addr)
    CHECK(alignof(ChiIpcReq) == 8, "ChiIpcReq alignment is 8");
}

// ─────────────────────────────────────────────────────────────────────────────
// 28. ChiIpcDat full field coverage (addr, db_id, resp_err, src_id, tgt_id)
// ─────────────────────────────────────────────────────────────────────────────

static void test_dat_full_fields()
{
    printf("\n── dat_full_fields ────────────────────────────────────────────\n");
    std::string name = shm_name(28);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    ChiIpcDat d{};
    d.addr      = 0xFEDCBA9876543000ULL;  // cache-line aligned
    d.opcode    = 0x04;     // CompData
    d.txn_id    = 0xAB;
    d.src_id    = 0x7F;     // max 7-bit NodeID in uint16_t
    d.tgt_id    = 0x55;
    d.home_n_id = 0x12;
    d.db_id     = 0x00FE;   // max TXNID_W=8 minus 1
    d.qos       = 0xD;
    d.resp      = 0x03;     // UC_Dirty
    d.resp_err  = 0x03;     // DERR
    d.data_id   = 1;
    d.data_len  = 64;
    for (int i = 0; i < 64; i++) {
        d.data[i] = static_cast<uint8_t>(0x80 ^ i);
        d.be[i]   = (i < 32) ? 0xFF : 0x00;  // first half enabled
    }

    lay->g2q_dat.push(d);
    ChiIpcDat out{};
    lay->g2q_dat.pop(out);

    CHECK(out.addr      == d.addr,      "dat_full: addr preserved");
    CHECK(out.opcode    == d.opcode,    "dat_full: opcode preserved");
    CHECK(out.txn_id    == d.txn_id,    "dat_full: txn_id preserved");
    CHECK(out.src_id    == d.src_id,    "dat_full: src_id preserved");
    CHECK(out.tgt_id    == d.tgt_id,    "dat_full: tgt_id preserved");
    CHECK(out.home_n_id == d.home_n_id, "dat_full: home_n_id preserved");
    CHECK(out.db_id     == d.db_id,     "dat_full: db_id preserved");
    CHECK(out.qos       == d.qos,       "dat_full: qos preserved");
    CHECK(out.resp      == d.resp,      "dat_full: resp preserved");
    CHECK(out.resp_err  == d.resp_err,  "dat_full: resp_err DERR preserved");
    CHECK(out.data_id   == d.data_id,   "dat_full: data_id=1 preserved");
    CHECK(out.data_len  == d.data_len,  "dat_full: data_len=64 preserved");

    bool data_ok = true, be_ok = true;
    for (int i = 0; i < 64; i++) {
        if (out.data[i] != static_cast<uint8_t>(0x80 ^ i)) data_ok = false;
        uint8_t exp_be = (i < 32) ? 0xFF : 0x00;
        if (out.be[i] != exp_be) be_ok = false;
    }
    CHECK(data_ok, "dat_full: data bytes preserved");
    CHECK(be_ok,   "dat_full: partial BE (first half) preserved");
}

// ─────────────────────────────────────────────────────────────────────────────
// 29. tryOpen called on already-open handle is a no-op (returns true, no double-open)
// ─────────────────────────────────────────────────────────────────────────────

static void test_tryopen_idempotent()
{
    printf("\n── tryopen_idempotent ─────────────────────────────────────────\n");
    std::string name = shm_name(29);
    ChiShmHandle creator(name, /*create=*/true);

    ChiShmHandle att(name, /*create=*/false);
    bool first = att.tryOpen();
    CHECK(first, "tryopen_idempotent: first tryOpen succeeds");
    CHECK(att.valid(), "tryopen_idempotent: valid after first open");

    ChiShmLayout* p1 = att.layout();
    bool second = att.tryOpen();
    CHECK(second, "tryopen_idempotent: second tryOpen returns true");
    CHECK(att.layout() == p1, "tryopen_idempotent: layout ptr unchanged on re-open");
}

// ─────────────────────────────────────────────────────────────────────────────
// 30. req.size max value (0x7, 3-bit) roundtrips without truncation
// ─────────────────────────────────────────────────────────────────────────────

static void test_req_size_max()
{
    printf("\n── req_size_max ───────────────────────────────────────────────\n");
    std::string name = shm_name(30);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    ChiIpcReq r{};
    r.size   = 0x7;   // 3-bit max (128 bytes — not valid CHI but IPC just passes it)
    r.opcode = 0x30;  // ReadNoSnp
    r.txn_id = 0xF0;

    lay->q2g_req.push(r);
    ChiIpcReq out{};
    lay->q2g_req.pop(out);

    CHECK(out.size   == 0x7,  "req_size_max: size=0x7 (3-bit max) preserved");
    CHECK(out.opcode == 0x30, "req_size_max: opcode preserved alongside");
    CHECK(out.txn_id == 0xF0, "req_size_max: txn_id preserved alongside");
}

// ─────────────────────────────────────────────────────────────────────────────
// 31. Bidirectional independence: q2g_req and g2q_rsp do not interfere
// ─────────────────────────────────────────────────────────────────────────────

static void test_bidirectional_independence()
{
    printf("\n── bidirectional_independence ─────────────────────────────────\n");
    std::string name = shm_name(31);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    // Push 5 REQs on the q2g path
    ChiIpcReq req{};
    req.opcode = 0x01;
    for (int i = 0; i < 5; i++) {
        req.txn_id = static_cast<uint8_t>(i + 0x10);
        req.addr   = 0x1000 + static_cast<uint64_t>(i) * 64;
        lay->q2g_req.push(req);
    }

    // Push 5 RSPs on the g2q path (opposite direction)
    ChiIpcRsp rsp{};
    rsp.opcode = 0x04;
    for (int i = 0; i < 5; i++) {
        rsp.txn_id = static_cast<uint8_t>(i + 0x20);
        rsp.db_id  = static_cast<uint16_t>(i + 0x30);
        lay->g2q_rsp.push(rsp);
    }

    // Drain and verify both directions independently
    int req_cnt = 0, rsp_cnt = 0;
    bool req_ok = true, rsp_ok = true;
    ChiIpcReq ro{};
    ChiIpcRsp rs{};
    while (lay->q2g_req.pop(ro)) {
        if (ro.txn_id != static_cast<uint8_t>(req_cnt + 0x10)) req_ok = false;
        req_cnt++;
    }
    while (lay->g2q_rsp.pop(rs)) {
        if (rs.txn_id != static_cast<uint8_t>(rsp_cnt + 0x20)) rsp_ok = false;
        if (rs.db_id  != static_cast<uint16_t>(rsp_cnt + 0x30)) rsp_ok = false;
        rsp_cnt++;
    }

    CHECK(req_cnt == 5,  "bidir: q2g_req: 5 REQs received");
    CHECK(rsp_cnt == 5,  "bidir: g2q_rsp: 5 RSPs received");
    CHECK(req_ok,        "bidir: q2g_req: txn_ids in order");
    CHECK(rsp_ok,        "bidir: g2q_rsp: fields in order");
    CHECK(lay->q2g_req.empty(), "bidir: q2g_req empty after test");
    CHECK(lay->g2q_rsp.empty(), "bidir: g2q_rsp empty after test");
}

// ─────────────────────────────────────────────────────────────────────────────
// 32. src_id wider than 7-bit NodeID (uint16_t can hold 0x1234)
// ─────────────────────────────────────────────────────────────────────────────

static void test_src_id_wide()
{
    printf("\n── src_id_wide ────────────────────────────────────────────────\n");
    std::string name = shm_name(32);
    ChiShmHandle h(name, /*create=*/true);
    ChiShmLayout* lay = h.layout();

    // RSP struct has src_id as uint16_t; verify it holds >7-bit values
    ChiIpcRsp r{};
    r.opcode = 0x04;
    r.src_id = 0x1234;  // wider than 7-bit NodeID
    r.tgt_id = 0x5678;
    lay->g2q_rsp.push(r);
    ChiIpcRsp out{};
    lay->g2q_rsp.pop(out);
    CHECK(out.src_id == 0x1234, "src_id_wide: RSP src_id=0x1234 preserved (uint16_t)");
    CHECK(out.tgt_id == 0x5678, "src_id_wide: RSP tgt_id=0x5678 preserved (uint16_t)");

    // REQ struct has src_id as uint16_t too
    ChiIpcReq req{};
    req.src_id = 0xABCD;
    req.tgt_id = 0xEF01;
    lay->q2g_req.push(req);
    ChiIpcReq rout{};
    lay->q2g_req.pop(rout);
    CHECK(rout.src_id == 0xABCD, "src_id_wide: REQ src_id=0xABCD preserved");
    CHECK(rout.tgt_id == 0xEF01, "src_id_wide: REQ tgt_id=0xEF01 preserved");
}

// ─────────────────────────────────────────────────────────────────────────────
// 33. create=true over a stale segment (shm_unlink-on-entry path)
// ─────────────────────────────────────────────────────────────────────────────

static void test_create_over_stale()
{
    printf("\n── create_over_stale ──────────────────────────────────────────\n");
    std::string name = shm_name(33);

    // First creation — write a sentinel
    {
        ChiShmHandle first(name, /*create=*/true);
        first.layout()->gem5_grant_ps.store(0xDEADC0DE);
        // Destructor calls shm_unlink
    }

    // Second creation — should silently unlink any remnant and start fresh
    // (The destructor of 'first' already unlinked it, but the constructor
    //  also calls shm_unlink proactively before opening — test that path
    //  works even if the segment still exists from a crash scenario.)
    //
    // Simulate a "crash remnant" by manually re-creating the segment
    // without mapping it (just leave a stale file-descriptor-less entry).
    shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);  // stale ghost

    {
        ChiShmHandle second(name, /*create=*/true);
        CHECK(second.valid(), "create_over_stale: second create succeeds");
        CHECK(second.layout() != nullptr, "create_over_stale: layout ptr valid");
        // The stale sentinel must NOT be visible (fresh shm is zero-initialized)
        CHECK(second.layout()->gem5_grant_ps.load() == 0,
              "create_over_stale: fresh shm is zero-initialised (no stale data)");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main()
{
    printf("=== test_ipc ===\n");

    test_lifecycle();
    test_ringbuf_basic();
    test_ringbuf_full();
    test_ringbuf_wraparound();
    test_time_markers();
    test_roundtrip_req();
    test_roundtrip_rsp();
    test_roundtrip_dat();
    test_roundtrip_snp();
    test_tryopen_retry();
    test_concurrent_all_channels();
    test_struct_sizes();
    test_req_all_fields();
    test_dat_capacity_32();
    test_dat_multibeat();
    test_dat_zero_len();
    test_snp_flag_combinations();
    test_two_segments_independent();
    test_ring_boundary_n_minus_1();
    test_head_tail_monotonic();
    test_shm_name_accessor();
    test_stress_high_volume();
    test_rsp_full_fields();
    test_interleaved_push_pop();
    test_dat_binary_pattern();
    test_layout_offset_zero();
    test_req_exp_comp_ack();
    test_rsp_fwd_state_data_pull();
    test_dat_cc_id();
    test_req_struct_size();
    test_dat_full_fields();
    test_tryopen_idempotent();
    test_req_size_max();
    test_bidirectional_independence();
    test_src_id_wide();
    test_create_over_stale();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
