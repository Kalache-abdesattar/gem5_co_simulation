// test_questa_top_ipc.cc
//
// Tests for questa_top_ipc: the IPC pin-level adapter for Questa co-sim.
//
// Part A — chi_flit_pack correctness for RESPERR and NS fields
//           (bugs that were silent before adding these checks)
// Part B — questa_top_ipc integration tests (clocked, shm-backed)
//           B.1  init credits are pulsed for CHI_INIT_LCRD cycles after reset
//           B.2  RSP RESPERR field propagated through g2q_rsp → rxrsp_flit
//           B.3  DAT RESPERR field propagated through g2q_dat → rxdat_flit
//           B.4  SNP NS field propagated through g2q_snp  → rxsnp_flit
//           B.5  DAT BE from m.be[] used (not synthesised all-ones)
//           B.6  REQ TX flit sampled → q2g_req (all fields round-trip)
//           B.7  RSP TX flit sampled → q2g_rsp (all fields round-trip)
//           B.8  DAT TX flit sampled → q2g_dat (data + BE round-trip)
//           B.9  questa_grant_ps advances each posedge
//           B.10 reset clears output queues and questa_ps

#include <systemc>
#include <chi/chi_tlm.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#include "chi_flit_pack.hh"
#include "questa_top_ipc.hh"
#include "../ipc/chi_ipc.hh"

using namespace gem5::ruby;
using namespace gem5::ruby::chi_ipc;
using namespace sc_core;
using namespace chi_flit;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { printf("  PASS: %s\n", msg); g_pass++; } \
        else      { printf("  FAIL: %s  (line %d)\n", msg, __LINE__); g_fail++; } \
    } while(0)

static std::string test_shm(int n) {
    static const pid_t pid = getpid();
    return "/chi_qt_ipc_" + std::to_string(pid) + "_" + std::to_string(n);
}

// ─────────────────────────────────────────────────────────────────────────────
// Part A — chi_flit_pack field correctness (no clock needed)
// ─────────────────────────────────────────────────────────────────────────────

static void test_pack_resperr_rsp()
{
    printf("\n── pack_rsp: RESPERR packed correctly ────────────────────────\n");
    chi::chi_ctrl_extension e;
    e.cmn.set_src_id(3);
    e.cmn.set_txn_id(0x10);
    e.resp.set_tgt_id(1);
    e.resp.set_opcode(chi::rsp_optype_e::Comp);
    e.resp.set_resp(chi::rsp_resptype_e::Comp_UC);
    e.resp.set_db_id(5);

    // Test all four resp_err values
    for (uint8_t err = 0; err < 4; err++) {
        e.resp.set_resp_err(static_cast<chi::rsp_resperrtype_e>(err));
        RspFlit f;
        pack_rsp(e, f);
        uint64_t got = bf_get(f, rsp_f::RESPERR_LO, rsp_f::RESPERR_W);
        CHECK(got == err, ("pack_rsp: RESPERR=" + std::to_string(err)
                           + " round-trips").c_str());
    }

    // Verify RESPERR and RESP bits do not overlap
    e.resp.set_resp_err(static_cast<chi::rsp_resperrtype_e>(2)); // EXOK=2
    e.resp.set_resp(chi::rsp_resptype_e::Comp_UC);
    RspFlit f;
    pack_rsp(e, f);
    CHECK(bf_get(f, rsp_f::RESPERR_LO, rsp_f::RESPERR_W) == 2,
          "pack_rsp: RESPERR=2 with resp set simultaneously");
    CHECK(bf_get(f, rsp_f::RESP_LO, rsp_f::RESP_W) ==
          static_cast<uint64_t>(chi::rsp_resptype_e::Comp_UC),
          "pack_rsp: RESP not corrupted by RESPERR");
}

static void test_pack_resperr_dat()
{
    printf("\n── pack_dat: RESPERR packed correctly ────────────────────────\n");
    chi::chi_data_extension e;
    e.cmn.set_src_id(5);
    e.cmn.set_txn_id(0x20);
    e.dat.set_tgt_id(2);
    e.dat.set_opcode(chi::dat_optype_e::CompData);
    e.dat.set_resp(chi::dat_resptype_e::CompData_UC);
    e.dat.set_data_id(0);

    uint8_t data[CHI_DATA_W_BYTES] = {};

    for (uint8_t err = 0; err < 4; err++) {
        e.dat.set_resp_err(static_cast<chi::rsp_resperrtype_e>(err));
        DatFlit f;
        pack_dat(e, data, CHI_DATA_W_BYTES, f);
        uint64_t got = bf_get(f, dat_f::RESPERR_LO, dat_f::RESPERR_W);
        CHECK(got == err, ("pack_dat: RESPERR=" + std::to_string(err)
                           + " round-trips").c_str());
    }

    // Verify RESPERR and RESP bits do not overlap
    e.dat.set_resp_err(static_cast<chi::rsp_resperrtype_e>(3)); // DERR=3
    e.dat.set_resp(chi::dat_resptype_e::CompData_SC);
    DatFlit f;
    pack_dat(e, data, CHI_DATA_W_BYTES, f);
    CHECK(bf_get(f, dat_f::RESPERR_LO, dat_f::RESPERR_W) == 3,
          "pack_dat: RESPERR=3 with resp set simultaneously");
    CHECK(bf_get(f, dat_f::RESP_LO, dat_f::RESP_W) ==
          static_cast<uint64_t>(chi::dat_resptype_e::CompData_SC),
          "pack_dat: RESP not corrupted by RESPERR");
}

static void test_pack_snp_ns()
{
    printf("\n── pack_snp: NS field uses extension value ────────────────────\n");
    chi::chi_snp_extension e;
    e.cmn.set_src_id(1);
    e.cmn.set_txn_id(0x42);
    e.req.set_opcode(chi::snp_optype_e::SnpShared);
    uint64_t addr = 0x4000;

    // NS = false
    e.req.set_non_secure(false);
    SnpFlit f0;
    pack_snp(e, addr, f0);
    CHECK(!bf_getb(f0, snp_f::NS_LO), "pack_snp: NS=false preserved");

    // NS = true
    e.req.set_non_secure(true);
    SnpFlit f1;
    pack_snp(e, addr, f1);
    CHECK(bf_getb(f1, snp_f::NS_LO), "pack_snp: NS=true preserved");

    // Other fields not disturbed by NS change
    CHECK(bf_get(f1, snp_f::SRCID_LO, snp_f::SRCID_W) == 1,
          "pack_snp: SRCID unchanged when NS flips");
    CHECK(bf_get(f1, snp_f::TXNID_LO, snp_f::TXNID_W) == 0x42,
          "pack_snp: TXNID unchanged when NS flips");
}

static void test_unpack_resperr_rsp()
{
    printf("\n── unpack_rsp: RESPERR extracted correctly ───────────────────\n");
    RspFlit f = 0;
    using F = rsp_f;
    bf_set(f, F::OPCODE_LO, F::OPCODE_W,
           static_cast<uint64_t>(chi::rsp_optype_e::Comp));
    bf_set(f, F::RESPERR_LO, F::RESPERR_W, 2);  // EXOK
    bf_set(f, F::RESP_LO, F::RESP_W,
           static_cast<uint64_t>(chi::rsp_resptype_e::Comp_UC));

    tlm::tlm_generic_payload gp;
    chi::chi_ctrl_extension e;
    unpack_rsp(f, gp, e);

    CHECK(static_cast<uint8_t>(e.resp.get_resp_err()) == 2,
          "unpack_rsp: RESPERR=2 (EXOK) extracted");
    CHECK(e.resp.get_resp() == chi::rsp_resptype_e::Comp_UC,
          "unpack_rsp: RESP not corrupted by RESPERR");
}

static void test_unpack_resperr_dat()
{
    printf("\n── unpack_dat: RESPERR extracted correctly ───────────────────\n");
    DatFlit f = 0;
    using F = dat_f;
    bf_set(f, F::OPCODE_LO, F::OPCODE_W,
           static_cast<uint64_t>(chi::dat_optype_e::CompData));
    bf_set(f, F::RESPERR_LO, F::RESPERR_W, 3);  // DERR
    bf_set(f, F::RESP_LO, F::RESP_W,
           static_cast<uint64_t>(chi::dat_resptype_e::CompData_SC));

    tlm::tlm_generic_payload gp;
    chi::chi_data_extension e;
    uint8_t buf[CHI_DATA_W_BYTES] = {};
    unpack_dat(f, gp, e, buf);

    CHECK(static_cast<uint8_t>(e.dat.get_resp_err()) == 3,
          "unpack_dat: RESPERR=3 (DERR) extracted");
    CHECK(e.dat.get_resp() == chi::dat_resptype_e::CompData_SC,
          "unpack_dat: RESP not corrupted by RESPERR");
}

// ─────────────────────────────────────────────────────────────────────────────
// Part B helpers
// ─────────────────────────────────────────────────────────────────────────────

// Advance exactly n full clock posedges (1000 ps each).
static void tick(int n = 1) { sc_start(n * 1000, SC_PS); }

// Bind all 22 ports of questa_top_ipc to a set of sc_signals.
struct TopPorts {
    sc_signal<bool>    rstn{"rstn", false};

    sc_signal<bool>    txreq_flitv   {"txreq_flitv",    false};
    sc_signal<bool>    txreq_flitpend{"txreq_flitpend",  false};
    sc_signal<ReqFlit> txreq_flit    {"txreq_flit"};
    sc_signal<bool>    txreq_lcrdv   {"txreq_lcrdv",    false};

    sc_signal<bool>    txrsp_flitv   {"txrsp_flitv",    false};
    sc_signal<bool>    txrsp_flitpend{"txrsp_flitpend",  false};
    sc_signal<RspFlit> txrsp_flit    {"txrsp_flit"};
    sc_signal<bool>    txrsp_lcrdv   {"txrsp_lcrdv",    false};

    sc_signal<bool>    txdat_flitv   {"txdat_flitv",    false};
    sc_signal<bool>    txdat_flitpend{"txdat_flitpend",  false};
    sc_signal<DatFlit> txdat_flit    {"txdat_flit"};
    sc_signal<bool>    txdat_lcrdv   {"txdat_lcrdv",    false};

    sc_signal<bool>    rxrsp_flitv   {"rxrsp_flitv",    false};
    sc_signal<bool>    rxrsp_flitpend{"rxrsp_flitpend",  false};
    sc_signal<RspFlit> rxrsp_flit    {"rxrsp_flit"};
    sc_signal<bool>    rxrsp_lcrdv   {"rxrsp_lcrdv",    false};

    sc_signal<bool>    rxdat_flitv   {"rxdat_flitv",    false};
    sc_signal<bool>    rxdat_flitpend{"rxdat_flitpend",  false};
    sc_signal<DatFlit> rxdat_flit    {"rxdat_flit"};
    sc_signal<bool>    rxdat_lcrdv   {"rxdat_lcrdv",    false};

    sc_signal<bool>    rxsnp_flitv   {"rxsnp_flitv",    false};
    sc_signal<bool>    rxsnp_flitpend{"rxsnp_flitpend",  false};
    sc_signal<SnpFlit> rxsnp_flit    {"rxsnp_flit"};
    sc_signal<bool>    rxsnp_lcrdv   {"rxsnp_lcrdv",    false};
};

static void bind(questa_top_ipc& qt, TopPorts& p)
{
    qt.rstn           (p.rstn);
    qt.txreq_flitv    (p.txreq_flitv);
    qt.txreq_flitpend (p.txreq_flitpend);
    qt.txreq_flit     (p.txreq_flit);
    qt.txreq_lcrdv    (p.txreq_lcrdv);
    qt.txrsp_flitv    (p.txrsp_flitv);
    qt.txrsp_flitpend (p.txrsp_flitpend);
    qt.txrsp_flit     (p.txrsp_flit);
    qt.txrsp_lcrdv    (p.txrsp_lcrdv);
    qt.txdat_flitv    (p.txdat_flitv);
    qt.txdat_flitpend (p.txdat_flitpend);
    qt.txdat_flit     (p.txdat_flit);
    qt.txdat_lcrdv    (p.txdat_lcrdv);
    qt.rxrsp_flitv    (p.rxrsp_flitv);
    qt.rxrsp_flitpend (p.rxrsp_flitpend);
    qt.rxrsp_flit     (p.rxrsp_flit);
    qt.rxrsp_lcrdv    (p.rxrsp_lcrdv);
    qt.rxdat_flitv    (p.rxdat_flitv);
    qt.rxdat_flitpend (p.rxdat_flitpend);
    qt.rxdat_flit     (p.rxdat_flit);
    qt.rxdat_lcrdv    (p.rxdat_lcrdv);
    qt.rxsnp_flitv    (p.rxsnp_flitv);
    qt.rxsnp_flitpend (p.rxsnp_flitpend);
    qt.rxsnp_flit     (p.rxsnp_flit);
    qt.rxsnp_lcrdv    (p.rxsnp_lcrdv);
}

// ─────────────────────────────────────────────────────────────────────────────
// B.1 — Init L-credits are pulsed for CHI_INIT_LCRD cycles after reset
// ─────────────────────────────────────────────────────────────────────────────

// B.1 uses the shared TopPorts; the module must already be created and bound.
static void test_b1_init_credits(sc_clock& clk, TopPorts& p)
{
    printf("\n── B.1 init_credits ───────────────────────────────────────────\n");

    // 5 cycles in reset
    p.rstn.write(false);
    tick(5);

    // After reset de-assertion, credits should fire for CHI_INIT_LCRD cycles
    p.rstn.write(true);

    int req_crd = 0, rsp_crd = 0, dat_crd = 0;
    for (int i = 0; i < CHI_INIT_LCRD + 5; i++) {
        tick(1);
        if (p.txreq_lcrdv.read()) req_crd++;
        if (p.txrsp_lcrdv.read()) rsp_crd++;
        if (p.txdat_lcrdv.read()) dat_crd++;
    }
    CHECK(req_crd == CHI_INIT_LCRD, "B.1: txreq_lcrdv pulsed CHI_INIT_LCRD times");
    CHECK(rsp_crd == CHI_INIT_LCRD, "B.1: txrsp_lcrdv pulsed CHI_INIT_LCRD times");
    CHECK(dat_crd == CHI_INIT_LCRD, "B.1: txdat_lcrdv pulsed CHI_INIT_LCRD times");
}

// ─────────────────────────────────────────────────────────────────────────────
// B.2 — RSP RESPERR propagated from g2q_rsp through ipc_to_rsp_flit
// ─────────────────────────────────────────────────────────────────────────────

static void test_b2_rsp_resperr(sc_clock& clk, ChiShmHandle& shm, TopPorts& p)
{
    printf("\n── B.2 rsp_resperr ────────────────────────────────────────────\n");

    // Reset credit counter: grant one rxrsp credit
    p.rxrsp_lcrdv.write(true);
    tick(1);
    p.rxrsp_lcrdv.write(false);

    // Push RSP with RESPERR=3 (DERR)
    ChiIpcRsp rsp{};
    rsp.opcode   = 0x04;  // Comp
    rsp.txn_id   = 0x11;
    rsp.src_id   = 5;
    rsp.tgt_id   = 10;
    rsp.resp     = 0x01;  // UC
    rsp.resp_err = 3;     // DERR
    rsp.db_id    = 0x22;
    shm.layout()->g2q_rsp.push(rsp);

    // One clock cycle: drain g2q_rsp → rsp_out_q_, drive to DUT (credit=1)
    tick(1);

    CHECK(p.rxrsp_flitv.read(), "B.2: rxrsp_flitv high");
    if (p.rxrsp_flitv.read()) {
        RspFlit f = p.rxrsp_flit.read();
        uint64_t got_err  = bf_get(f, rsp_f::RESPERR_LO, rsp_f::RESPERR_W);
        uint64_t got_resp = bf_get(f, rsp_f::RESP_LO,    rsp_f::RESP_W);
        uint64_t got_txnid= bf_get(f, rsp_f::TXNID_LO,   rsp_f::TXNID_W);
        CHECK(got_err   == 3,    "B.2: RESPERR=3 in rxrsp_flit");
        CHECK(got_resp  == 0x01, "B.2: RESP=UC in rxrsp_flit");
        CHECK(got_txnid == 0x11, "B.2: TxnID in rxrsp_flit");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// B.3 — DAT RESPERR propagated from g2q_dat through ipc_to_dat_flit
// ─────────────────────────────────────────────────────────────────────────────

static void test_b3_dat_resperr(sc_clock& clk, ChiShmHandle& shm, TopPorts& p)
{
    printf("\n── B.3 dat_resperr ────────────────────────────────────────────\n");

    p.rxdat_lcrdv.write(true);
    tick(1);
    p.rxdat_lcrdv.write(false);

    ChiIpcDat dat{};
    dat.opcode   = 0x04;  // CompData
    dat.txn_id   = 0x33;
    dat.src_id   = 6;
    dat.tgt_id   = 11;
    dat.resp     = 0x02;  // SC
    dat.resp_err = 2;     // EXOK
    dat.data_len = 64;
    for (int i = 0; i < 64; i++) { dat.data[i] = static_cast<uint8_t>(i); dat.be[i] = 0xFF; }
    shm.layout()->g2q_dat.push(dat);

    tick(1);

    CHECK(p.rxdat_flitv.read(), "B.3: rxdat_flitv high");
    if (p.rxdat_flitv.read()) {
        DatFlit f = p.rxdat_flit.read();
        uint64_t got_err   = bf_get(f, dat_f::RESPERR_LO, dat_f::RESPERR_W);
        uint64_t got_resp  = bf_get(f, dat_f::RESP_LO,    dat_f::RESP_W);
        uint64_t got_txnid = bf_get(f, dat_f::TXNID_LO,   dat_f::TXNID_W);
        CHECK(got_err   == 2,    "B.3: RESPERR=2 in rxdat_flit");
        CHECK(got_resp  == 0x02, "B.3: RESP=SC in rxdat_flit");
        CHECK(got_txnid == 0x33, "B.3: TxnID in rxdat_flit");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// B.4 — SNP NS propagated from g2q_snp through ipc_to_snp_flit
// ─────────────────────────────────────────────────────────────────────────────

static void test_b4_snp_ns(sc_clock& clk, ChiShmHandle& shm, TopPorts& p)
{
    printf("\n── B.4 snp_ns ─────────────────────────────────────────────────\n");

    // Grant snp credit
    p.rxsnp_lcrdv.write(true);
    tick(1);
    p.rxsnp_lcrdv.write(false);

    // First: SNP with ns=1
    ChiIpcSnp snp{};
    snp.opcode  = 0x01;  // SnpShared
    snp.txn_id  = 0x44;
    snp.src_id  = 7;
    snp.addr    = 0x8000;
    snp.ns      = 1;
    shm.layout()->g2q_snp.push(snp);

    tick(1);

    CHECK(p.rxsnp_flitv.read(), "B.4: rxsnp_flitv high (ns=1)");
    if (p.rxsnp_flitv.read()) {
        SnpFlit f = p.rxsnp_flit.read();
        CHECK(bf_getb(f, snp_f::NS_LO), "B.4: NS=1 in rxsnp_flit");
    }

    // Second: SNP with ns=0 — grant another credit
    p.rxsnp_lcrdv.write(true);
    tick(1);
    p.rxsnp_lcrdv.write(false);

    snp.ns = 0;
    shm.layout()->g2q_snp.push(snp);

    tick(1);

    CHECK(p.rxsnp_flitv.read(), "B.4: rxsnp_flitv high (ns=0)");
    if (p.rxsnp_flitv.read()) {
        SnpFlit f = p.rxsnp_flit.read();
        CHECK(!bf_getb(f, snp_f::NS_LO), "B.4: NS=0 in rxsnp_flit");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// B.5 — DAT BE bytes from m.be[] used in flit, not synthesised all-ones
// ─────────────────────────────────────────────────────────────────────────────

static void test_b5_dat_be(sc_clock& clk, ChiShmHandle& shm, TopPorts& p)
{
    printf("\n── B.5 dat_be_from_ipc_struct ─────────────────────────────────\n");

    p.rxdat_lcrdv.write(true);
    tick(1);
    p.rxdat_lcrdv.write(false);

    ChiIpcDat dat{};
    dat.opcode   = 0x04;
    dat.txn_id   = 0x55;
    dat.data_len = 64;
    // Partial BE: even bytes enabled, odd bytes disabled
    for (int i = 0; i < 64; i++) {
        dat.data[i] = static_cast<uint8_t>(i + 1);
        dat.be[i]   = (i % 2 == 0) ? 0xFF : 0x00;
    }
    shm.layout()->g2q_dat.push(dat);

    tick(1);

    CHECK(p.rxdat_flitv.read(), "B.5: rxdat_flitv high");
    if (p.rxdat_flitv.read()) {
        DatFlit f = p.rxdat_flit.read();
        bool be_ok = true;
        for (int i = 0; i < 64; i++) {
            bool expected = (i % 2 == 0);
            bool got      = bf_getb(f, dat_f::BE_LO + i);
            if (got != expected) be_ok = false;
        }
        CHECK(be_ok, "B.5: partial BE from m.be[] matches flit BE bits");

        // Also verify even-indexed data bytes match
        bool data_ok = true;
        for (int i = 0; i < 64; i += 2) {
            uint8_t got = static_cast<uint8_t>(
                f.range(dat_f::DATA_LO + i*8 + 7, dat_f::DATA_LO + i*8).to_uint());
            if (got != static_cast<uint8_t>(i + 1)) data_ok = false;
        }
        CHECK(data_ok, "B.5: data bytes in flit match m.data[]");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// B.6 — REQ TX flit → q2g_req round-trip (all major fields)
// ─────────────────────────────────────────────────────────────────────────────

static void test_b6_req_flit_to_ipc(sc_clock& clk, ChiShmHandle& shm, TopPorts& p)
{
    printf("\n── B.6 req_flit_to_ipc ────────────────────────────────────────\n");

    // Build a REQ flit with known fields
    ReqFlit f = 0;
    using F = req_f;
    bf_set(f, F::OPCODE_LO,      F::OPCODE_W,  0x01);   // ReadShared
    bf_set(f, F::TXNID_LO,       F::TXNID_W,   0xAB);
    bf_set(f, F::SRCID_LO,       F::SRCID_W,   9);
    bf_set(f, F::TGTID_LO,       F::TGTID_W,   3);
    bf_set(f, F::ADDR_LO,        F::ADDR_W,    0x12340);
    bf_set(f, F::QOS_LO,         F::QOS_W,     5);
    bf_setb(f, F::NS_LO,         true);
    bf_setb(f, F::ALLOWRETRY_LO, true);
    bf_set(f, F::ORDER_LO,       F::ORDER_W,   2);
    bf_set(f, F::SIZE_LO,        F::SIZE_W,    6);
    bf_set(f, F::MEMATTR_LO,     F::MEMATTR_W, 0xA);
    bf_setb(f, F::SNOOPME_LO,    true);
    bf_set(f, F::PCRDTYPE_LO,    F::PCRDTYPE_W,0x7);

    p.txreq_flit.write(f);
    p.txreq_flitv.write(true);
    tick(1);
    // txreq_lcrdv is returned in the SAME posedge that sampled the flit.
    // Check it before de-asserting flitv (next tick would clear it).
    CHECK(p.txreq_lcrdv.read(), "B.6: txreq_lcrdv high after receiving flit");
    p.txreq_flitv.write(false);
    tick(1);

    // Check q2g_req has the message
    CHECK(!shm.layout()->q2g_req.empty(), "B.6: q2g_req non-empty after TX flit");

    ChiIpcReq out{};
    shm.layout()->q2g_req.pop(out);
    CHECK(out.opcode      == 0x01,    "B.6: REQ opcode");
    CHECK(out.txn_id      == 0xAB,    "B.6: REQ txn_id");
    CHECK(out.src_id      == 9,       "B.6: REQ src_id");
    CHECK(out.tgt_id      == 3,       "B.6: REQ tgt_id");
    CHECK(out.addr        == 0x12340, "B.6: REQ addr");
    CHECK(out.qos         == 5,       "B.6: REQ qos");
    CHECK(out.ns          == 1,       "B.6: REQ ns");
    CHECK(out.allow_retry == 1,       "B.6: REQ allow_retry");
    CHECK(out.order       == 2,       "B.6: REQ order");
    CHECK(out.size        == 6,       "B.6: REQ size");
    CHECK(out.mem_attr    == 0xA,     "B.6: REQ mem_attr");
    CHECK(out.exc_l       == 1,       "B.6: REQ exc_l (same bit as snoop_me)");
    CHECK(out.snoop_me    == 1,       "B.6: REQ snoop_me");
    CHECK(out.pcrd_type   == 0x7,     "B.6: REQ pcrd_type");
}

// ─────────────────────────────────────────────────────────────────────────────
// B.7 — RSP TX flit → q2g_rsp round-trip
// ─────────────────────────────────────────────────────────────────────────────

static void test_b7_rsp_flit_to_ipc(sc_clock& clk, ChiShmHandle& shm, TopPorts& p)
{
    printf("\n── B.7 rsp_flit_to_ipc ────────────────────────────────────────\n");

    RspFlit f = 0;
    using F = rsp_f;
    bf_set(f, F::OPCODE_LO,   F::OPCODE_W,  0x06);  // CompAck
    bf_set(f, F::TXNID_LO,    F::TXNID_W,   0xCC);
    bf_set(f, F::SRCID_LO,    F::SRCID_W,   4);
    bf_set(f, F::TGTID_LO,    F::TGTID_W,   2);
    bf_set(f, F::QOS_LO,      F::QOS_W,     3);
    bf_set(f, F::RESP_LO,     F::RESP_W,    0x01);  // UC
    bf_set(f, F::RESPERR_LO,  F::RESPERR_W, 1);     // NDERR
    bf_set(f, F::DBID_LO,     F::DBID_W,    0x0F);
    bf_set(f, F::PCRDTYPE_LO, F::PCRDTYPE_W,0x5);

    p.txrsp_flit.write(f);
    p.txrsp_flitv.write(true);
    tick(1);
    p.txrsp_flitv.write(false);
    tick(1);

    CHECK(!shm.layout()->q2g_rsp.empty(), "B.7: q2g_rsp non-empty after TX flit");

    ChiIpcRsp out{};
    shm.layout()->q2g_rsp.pop(out);
    CHECK(out.opcode    == 0x06,  "B.7: RSP opcode CompAck");
    CHECK(out.txn_id    == 0xCC,  "B.7: RSP txn_id");
    CHECK(out.src_id    == 4,     "B.7: RSP src_id");
    CHECK(out.tgt_id    == 2,     "B.7: RSP tgt_id");
    CHECK(out.qos       == 3,     "B.7: RSP qos");
    CHECK(out.resp      == 0x01,  "B.7: RSP resp UC");
    CHECK(out.resp_err  == 1,     "B.7: RSP resp_err NDERR");
    CHECK(out.db_id     == 0x0F,  "B.7: RSP db_id");
    CHECK(out.pcrd_type == 0x5,   "B.7: RSP pcrd_type");
}

// ─────────────────────────────────────────────────────────────────────────────
// B.8 — DAT TX flit → q2g_dat round-trip (data + BE)
// ─────────────────────────────────────────────────────────────────────────────

static void test_b8_dat_flit_to_ipc(sc_clock& clk, ChiShmHandle& shm, TopPorts& p)
{
    printf("\n── B.8 dat_flit_to_ipc ────────────────────────────────────────\n");

    DatFlit f = 0;
    using F = dat_f;
    bf_set(f, F::OPCODE_LO,  F::OPCODE_W,  0x02);  // CopyBackWrData
    bf_set(f, F::TXNID_LO,   F::TXNID_W,   0x77);
    bf_set(f, F::SRCID_LO,   F::SRCID_W,   8);
    bf_set(f, F::TGTID_LO,   F::TGTID_W,   5);
    bf_set(f, F::HOMEID_LO,  F::HOMEID_W,  2);
    bf_set(f, F::RESP_LO,    F::RESP_W,    0x02);  // SC
    bf_set(f, F::RESPERR_LO, F::RESPERR_W, 0);
    bf_set(f, F::DATAID_LO,  F::DATAID_W,  1);

    // Fill data bytes and BE (alternating pattern)
    for (int i = 0; i < CHI_DATA_W_BYTES; i++) {
        bf_set(f, F::DATA_LO + i*8, 8, static_cast<uint64_t>(i ^ 0x5A));
        bf_setb(f, F::BE_LO + i, (i % 4 != 3));  // every 4th byte disabled
    }

    p.txdat_flit.write(f);
    p.txdat_flitv.write(true);
    tick(1);
    p.txdat_flitv.write(false);
    tick(1);

    CHECK(!shm.layout()->q2g_dat.empty(), "B.8: q2g_dat non-empty after TX flit");

    ChiIpcDat out{};
    shm.layout()->q2g_dat.pop(out);
    CHECK(out.opcode   == 0x02,  "B.8: DAT opcode CopyBackWrData");
    CHECK(out.txn_id   == 0x77,  "B.8: DAT txn_id");
    CHECK(out.src_id   == 8,     "B.8: DAT src_id");
    CHECK(out.tgt_id   == 5,     "B.8: DAT tgt_id");
    CHECK(out.home_n_id== 2,     "B.8: DAT home_n_id");
    CHECK(out.resp     == 0x02,  "B.8: DAT resp SC");
    CHECK(out.data_id  == 1,     "B.8: DAT data_id=1");
    CHECK(out.data_len == CHI_DATA_W_BYTES, "B.8: DAT data_len");

    bool data_ok = true, be_ok = true;
    for (int i = 0; i < CHI_DATA_W_BYTES; i++) {
        if (out.data[i] != static_cast<uint8_t>(i ^ 0x5A)) data_ok = false;
        bool exp_be = (i % 4 != 3);
        if ((out.be[i] != 0) != exp_be) be_ok = false;
    }
    CHECK(data_ok, "B.8: data bytes round-trip through flit");
    CHECK(be_ok,   "B.8: BE bytes extracted from flit");
}

// ─────────────────────────────────────────────────────────────────────────────
// B.9 — questa_grant_ps advances monotonically each posedge
// ─────────────────────────────────────────────────────────────────────────────

static void test_b9_questa_ps(sc_clock& clk, ChiShmHandle& shm, TopPorts& p)
{
    printf("\n── B.9 questa_grant_ps ────────────────────────────────────────\n");

    uint64_t prev = shm.layout()->questa_grant_ps.load();
    bool monotonic = true;
    for (int i = 0; i < 10; i++) {
        tick(1);
        uint64_t cur = shm.layout()->questa_grant_ps.load();
        if (cur <= prev) { monotonic = false; break; }
        prev = cur;
    }
    CHECK(monotonic, "B.9: questa_grant_ps strictly increasing each posedge");
}

// ─────────────────────────────────────────────────────────────────────────────
// B.10 — Reset clears output queues: pending flits are dropped
// ─────────────────────────────────────────────────────────────────────────────

static void test_b10_reset(sc_clock& clk, ChiShmHandle& shm, TopPorts& p)
{
    printf("\n── B.10 reset_clears_queues ───────────────────────────────────\n");

    // Push 3 RSPs but give no credits so they queue up
    for (int i = 0; i < 3; i++) {
        ChiIpcRsp r{}; r.opcode = 4; r.txn_id = static_cast<uint8_t>(i);
        shm.layout()->g2q_rsp.push(r);
    }
    tick(2);  // drain g2q_rsp into rsp_out_q_, but no credit → flit not driven

    // Assert reset — queues should be cleared
    p.rstn.write(false);
    tick(2);

    // De-assert reset and grant a credit
    p.rstn.write(true);
    p.rxrsp_lcrdv.write(true);
    tick(1);
    p.rxrsp_lcrdv.write(false);
    tick(2);

    // The previously queued RSPs should have been dropped by the reset.
    // No flit should drive now (g2q_rsp is empty after reset drain).
    CHECK(!p.rxrsp_flitv.read(),
          "B.10: rxrsp_flitv low after reset (queued flits dropped)");
}

// ─────────────────────────────────────────────────────────────────────────────
// B.11 — REQ TX flit: exp_comp_ack bit extracted into q2g_req.exp_comp_ack
// ─────────────────────────────────────────────────────────────────────────────

static void test_b11_req_exp_comp_ack(sc_clock& clk, ChiShmHandle& shm, TopPorts& p)
{
    printf("\n── B.11 req_exp_comp_ack ──────────────────────────────────────\n");

    { chi_ipc::ChiIpcReq tmp; while (shm.layout()->q2g_req.pop(tmp)) {} }

    ReqFlit f1 = 0;
    bf_set(f1, req_f::OPCODE_LO,      req_f::OPCODE_W, 0x01);
    bf_set(f1, req_f::TXNID_LO,       req_f::TXNID_W,  0xD0);
    bf_set(f1, req_f::SRCID_LO,       req_f::SRCID_W,  4);
    bf_set(f1, req_f::SIZE_LO,        req_f::SIZE_W,   6);
    bf_setb(f1, req_f::EXPCOMPACK_LO, true);
    p.txreq_flit.write(f1); p.txreq_flitv.write(true);
    tick(1); p.txreq_flitv.write(false); tick(1);
    chi_ipc::ChiIpcReq out1{};
    shm.layout()->q2g_req.pop(out1);
    CHECK(out1.exp_comp_ack == 1,    "B.11: ExpCompAck=1 extracted into q2g_req");
    CHECK(out1.txn_id       == 0xD0, "B.11: txn_id not clobbered by ExpCompAck");

    ReqFlit f0 = 0;
    bf_set(f0, req_f::OPCODE_LO,      req_f::OPCODE_W, 0x30);
    bf_set(f0, req_f::TXNID_LO,       req_f::TXNID_W,  0xD1);
    bf_set(f0, req_f::SRCID_LO,       req_f::SRCID_W,  4);
    bf_set(f0, req_f::SIZE_LO,        req_f::SIZE_W,   6);
    bf_setb(f0, req_f::EXPCOMPACK_LO, false);
    p.txreq_flit.write(f0); p.txreq_flitv.write(true);
    tick(1); p.txreq_flitv.write(false); tick(1);
    chi_ipc::ChiIpcReq out0{};
    shm.layout()->q2g_req.pop(out0);
    CHECK(out0.exp_comp_ack == 0, "B.11: ExpCompAck=0 extracted correctly");
}

// ─────────────────────────────────────────────────────────────────────────────
// B.12 — RSP TX flit: fwd_state and data_pull extracted into q2g_rsp
// ─────────────────────────────────────────────────────────────────────────────

static void test_b12_rsp_fwd_state_q2g(sc_clock& clk, ChiShmHandle& shm, TopPorts& p)
{
    printf("\n── B.12 rsp_fwd_state_q2g ─────────────────────────────────────\n");

    { chi_ipc::ChiIpcRsp tmp; while (shm.layout()->q2g_rsp.pop(tmp)) {} }

    RspFlit f = 0;
    bf_set(f, rsp_f::OPCODE_LO,   rsp_f::OPCODE_W,   0x09);
    bf_set(f, rsp_f::TXNID_LO,    rsp_f::TXNID_W,    0xE0);
    bf_set(f, rsp_f::SRCID_LO,    rsp_f::SRCID_W,    4);
    bf_set(f, rsp_f::TGTID_LO,    rsp_f::TGTID_W,    2);
    bf_set(f, rsp_f::RESP_LO,     rsp_f::RESP_W,     0x01);
    bf_set(f, rsp_f::FWDSTATE_LO, rsp_f::FWDSTATE_W, 5);
    bf_setb(f, rsp_f::DATAPULL_LO, true);
    p.txrsp_flit.write(f); p.txrsp_flitv.write(true);
    tick(1); p.txrsp_flitv.write(false); tick(1);
    chi_ipc::ChiIpcRsp out{};
    shm.layout()->q2g_rsp.pop(out);
    CHECK(out.fwd_state == 5,    "B.12: fwd_state=5 extracted from RSP TX flit");
    CHECK(out.data_pull == 1,    "B.12: data_pull=1 extracted from RSP TX flit");
    CHECK(out.txn_id    == 0xE0, "B.12: txn_id not clobbered by new fields");
    CHECK(out.resp      == 0x01, "B.12: resp not clobbered");

    RspFlit f0 = 0;
    bf_set(f0, rsp_f::OPCODE_LO, rsp_f::OPCODE_W, 0x04);
    bf_set(f0, rsp_f::TXNID_LO,  rsp_f::TXNID_W,  0xE1);
    bf_set(f0, rsp_f::SRCID_LO,  rsp_f::SRCID_W,  4);
    p.txrsp_flit.write(f0); p.txrsp_flitv.write(true);
    tick(1); p.txrsp_flitv.write(false); tick(1);
    chi_ipc::ChiIpcRsp out0{};
    shm.layout()->q2g_rsp.pop(out0);
    CHECK(out0.fwd_state == 0, "B.12: fwd_state=0 (default) correct");
    CHECK(out0.data_pull == 0, "B.12: data_pull=0 (default) correct");
}

// ─────────────────────────────────────────────────────────────────────────────
// B.13 — DAT TX flit: cc_id extracted into q2g_dat (all 4 values)
// ─────────────────────────────────────────────────────────────────────────────

static void test_b13_dat_cc_id_q2g(sc_clock& clk, ChiShmHandle& shm, TopPorts& p)
{
    printf("\n── B.13 dat_cc_id_q2g ─────────────────────────────────────────\n");

    { chi_ipc::ChiIpcDat tmp; while (shm.layout()->q2g_dat.pop(tmp)) {} }

    for (int cc = 0; cc < 4; cc++) {
        DatFlit f = 0;
        bf_set(f, dat_f::OPCODE_LO, dat_f::OPCODE_W, 0x02);
        bf_set(f, dat_f::TXNID_LO,  dat_f::TXNID_W,  static_cast<uint64_t>(0xF0 + cc));
        bf_set(f, dat_f::SRCID_LO,  dat_f::SRCID_W,  4);
        bf_set(f, dat_f::CCID_LO,   dat_f::CCID_W,   static_cast<uint64_t>(cc));
        p.txdat_flit.write(f); p.txdat_flitv.write(true);
        tick(1); p.txdat_flitv.write(false); tick(1);
        chi_ipc::ChiIpcDat out{};
        shm.layout()->q2g_dat.pop(out);
        CHECK(out.cc_id  == static_cast<uint8_t>(cc),
              ("B.13: cc_id=" + std::to_string(cc) + " extracted from DAT TX flit").c_str());
        CHECK(out.txn_id == static_cast<uint8_t>(0xF0 + cc),
              "B.13: txn_id not clobbered by cc_id extraction");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// B.14 — g2q_rsp → rxrsp_flit: fwd_state and data_pull packed into flit
// ─────────────────────────────────────────────────────────────────────────────

static void test_b14_rsp_fwd_state_g2q(sc_clock& clk, ChiShmHandle& shm, TopPorts& p)
{
    printf("\n── B.14 rsp_fwd_state_g2q ─────────────────────────────────────\n");

    p.rxrsp_lcrdv.write(true); tick(1); p.rxrsp_lcrdv.write(false);

    chi_ipc::ChiIpcRsp r{};
    r.opcode    = 0x09;
    r.txn_id    = 0xA5;
    r.src_id    = 3;
    r.tgt_id    = 7;
    r.fwd_state = 6;
    r.data_pull = 1;
    shm.layout()->g2q_rsp.push(r);
    tick(1);

    CHECK(p.rxrsp_flitv.read(), "B.14: rxrsp_flitv high");
    if (p.rxrsp_flitv.read()) {
        RspFlit f = p.rxrsp_flit.read();
        uint64_t fs  = bf_get(f, rsp_f::FWDSTATE_LO, rsp_f::FWDSTATE_W);
        bool     dp  = bf_getb(f, rsp_f::DATAPULL_LO);
        uint64_t tid = bf_get(f, rsp_f::TXNID_LO, rsp_f::TXNID_W);
        CHECK(fs  == 6,    "B.14: fwd_state=6 packed into rxrsp_flit");
        CHECK(dp  == true, "B.14: data_pull=1 packed into rxrsp_flit");
        CHECK(tid == 0xA5, "B.14: txn_id not clobbered");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// B.15 — g2q_dat → rxdat_flit: cc_id packed into flit (all 4 values)
// ─────────────────────────────────────────────────────────────────────────────

static void test_b15_dat_cc_id_g2q(sc_clock& clk, ChiShmHandle& shm, TopPorts& p)
{
    printf("\n── B.15 dat_cc_id_g2q ─────────────────────────────────────────\n");

    for (int cc = 0; cc < 4; cc++) {
        p.rxdat_lcrdv.write(true); tick(1); p.rxdat_lcrdv.write(false);

        chi_ipc::ChiIpcDat d{};
        d.opcode   = 0x04;
        d.txn_id   = static_cast<uint8_t>(0xB0 + cc);
        d.src_id   = 5; d.tgt_id = 3;
        d.resp     = 0x02;
        d.data_len = 64;
        d.cc_id    = static_cast<uint8_t>(cc);
        for (int i = 0; i < 64; i++) { d.data[i] = 0x55; d.be[i] = 0xFF; }
        shm.layout()->g2q_dat.push(d);
        tick(1);

        CHECK(p.rxdat_flitv.read(), "B.15: rxdat_flitv high");
        if (p.rxdat_flitv.read()) {
            DatFlit f    = p.rxdat_flit.read();
            uint64_t got_cc  = bf_get(f, dat_f::CCID_LO,  dat_f::CCID_W);
            uint64_t got_tid = bf_get(f, dat_f::TXNID_LO, dat_f::TXNID_W);
            CHECK(got_cc  == static_cast<uint64_t>(cc),
                  ("B.15: cc_id=" + std::to_string(cc) + " in rxdat_flit").c_str());
            CHECK(got_tid == static_cast<uint64_t>(0xB0 + cc),
                  "B.15: txn_id not clobbered");
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// B.16 — Four consecutive RSPs through g2q_rsp: order and txn_ids preserved
// ─────────────────────────────────────────────────────────────────────────────

static void test_b11_multi_rsp(sc_clock& clk, ChiShmHandle& shm, TopPorts& p)
{
    printf("\n── B.11 multi_rsp ─────────────────────────────────────────────\n");

    // Grant 4 rxrsp credits up front
    for (int i = 0; i < 4; i++) {
        p.rxrsp_lcrdv.write(true);
        tick(1);
    }
    p.rxrsp_lcrdv.write(false);

    // Push 4 RSPs with sequential txn_ids
    for (int i = 0; i < 4; i++) {
        ChiIpcRsp r{};
        r.opcode  = 0x04;           // Comp
        r.txn_id  = static_cast<uint8_t>(0x40 + i);
        r.src_id  = 5;
        r.tgt_id  = 10;
        r.resp    = 0x01;           // UC
        shm.layout()->g2q_rsp.push(r);
    }

    // 4 clock ticks: each cycle drains one RSP from g2q and drives one flit
    for (int i = 0; i < 4; i++) {
        tick(1);
        CHECK(p.rxrsp_flitv.read(),
              ("B.11: rxrsp_flitv high on flit " + std::to_string(i)).c_str());
        uint64_t tid = bf_get(p.rxrsp_flit.read(),
                              rsp_f::TXNID_LO, rsp_f::TXNID_W);
        CHECK(tid == static_cast<uint64_t>(0x40 + i),
              ("B.11: txn_id=" + std::to_string(0x40 + i) + " in order").c_str());
    }

    // After 4 flits the RSP queue should be empty → no more flits
    tick(1);
    CHECK(!p.rxrsp_flitv.read(),
          "B.11: rxrsp_flitv low after all 4 RSPs consumed");
}

// ─────────────────────────────────────────────────────────────────────────────
// B.12 — REQ opcode diversity: ReadShared / ReadNoSnp / WriteNoSnpFull all
//         appear in q2g_req with correct opcodes after being driven on TX
// ─────────────────────────────────────────────────────────────────────────────

static void test_b12_req_opcodes(sc_clock& clk, ChiShmHandle& shm, TopPorts& p)
{
    printf("\n── B.12 req_opcodes ───────────────────────────────────────────\n");

    // Drain any stale entries from q2g_req first
    { chi_ipc::ChiIpcReq tmp; while (shm.layout()->q2g_req.pop(tmp)) {} }

    // Three opcodes: ReadShared=0x01, ReadNoSnp=0x30, WriteNoSnpFull=0x17
    const struct { uint8_t op; const char* name; } cases[] = {
        {0x01, "ReadShared"},
        {0x30, "ReadNoSnp"},
        {0x17, "WriteNoSnpFull"},
    };

    for (auto& c : cases) {
        ReqFlit flit{};
        bf_set(flit, req_f::OPCODE_LO, req_f::OPCODE_W, c.op);
        bf_set(flit, req_f::TXNID_LO,  req_f::TXNID_W,  0x55);
        bf_set(flit, req_f::SRCID_LO,  req_f::SRCID_W,  4);
        bf_set(flit, req_f::SIZE_LO,   req_f::SIZE_W,   6);  // 64 bytes
        bf_set(flit, req_f::ADDR_LO,   req_f::ADDR_W,   0x1000 >> 0);

        p.txreq_flit.write(flit);
        p.txreq_flitv.write(true);
        tick(1);
        p.txreq_flitv.write(false);
        tick(1);

        chi_ipc::ChiIpcReq got{};
        bool ok = shm.layout()->q2g_req.pop(got);
        CHECK(ok,            (std::string("B.12: ") + c.name + " pushed to q2g_req").c_str());
        CHECK(got.opcode == c.op,
              (std::string("B.12: ") + c.name + " opcode preserved in q2g_req").c_str());
        CHECK(got.txn_id == 0x55,
              (std::string("B.12: ") + c.name + " txn_id=0x55 preserved").c_str());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// B.13 — SNP ret_to_src and do_not_goto_sd propagate from g2q_snp → rxsnp_flit
// ─────────────────────────────────────────────────────────────────────────────

static void test_b13_snp_flags(sc_clock& clk, ChiShmHandle& shm, TopPorts& p)
{
    printf("\n── B.13 snp_flags ─────────────────────────────────────────────\n");

    // Test all 4 combinations of {ret_to_src, do_not_goto_sd}
    const struct { uint8_t rts; uint8_t dngs; } combos[] = {
        {0, 0},
        {1, 0},
        {0, 1},
        {1, 1},
    };

    // Grant 4 rxsnp credits
    for (int i = 0; i < 4; i++) {
        p.rxsnp_lcrdv.write(true);
        tick(1);
    }
    p.rxsnp_lcrdv.write(false);

    for (auto& c : combos) {
        chi_ipc::ChiIpcSnp snp{};
        snp.opcode      = 0x01;  // SnpShared
        snp.txn_id      = 0xC0;
        snp.src_id      = 3;
        snp.addr        = 0x9000;
        snp.ret_to_src  = c.rts;
        snp.do_not_goto_sd = c.dngs;
        shm.layout()->g2q_snp.push(snp);
    }

    for (auto& c : combos) {
        tick(1);
        CHECK(p.rxsnp_flitv.read(), "B.13: rxsnp_flitv high");
        if (p.rxsnp_flitv.read()) {
            SnpFlit f = p.rxsnp_flit.read();
            bool rts  = bf_getb(f, snp_f::RETTOSRC_LO);
            bool dngs = bf_getb(f, snp_f::DONOTGOTOSD_LO);
            CHECK(static_cast<int>(rts)  == c.rts,
                  ("B.13: ret_to_src=" + std::to_string(c.rts) + " in flit").c_str());
            CHECK(static_cast<int>(dngs) == c.dngs,
                  ("B.13: do_not_goto_sd=" + std::to_string(c.dngs) + " in flit").c_str());
        }
    }

    tick(1);
    CHECK(!p.rxsnp_flitv.read(), "B.13: rxsnp_flitv low after all 4 SNPs");
}

// ─────────────────────────────────────────────────────────────────────────────
// B.14 — DAT txn_id=0xFF boundary (max TXNID_W=8 value) in rxdat_flit
// ─────────────────────────────────────────────────────────────────────────────

static void test_b14_dat_txnid_max(sc_clock& clk, ChiShmHandle& shm, TopPorts& p)
{
    printf("\n── B.14 dat_txnid_max ─────────────────────────────────────────\n");

    p.rxdat_lcrdv.write(true);
    tick(1);
    p.rxdat_lcrdv.write(false);

    chi_ipc::ChiIpcDat dat{};
    dat.opcode   = 0x04;   // CompData
    dat.txn_id   = 0xFF;   // max 8-bit TXNID
    dat.src_id   = 7;
    dat.tgt_id   = 4;
    dat.resp     = 0x02;   // UC
    dat.data_len = 64;
    for (int i = 0; i < 64; i++) { dat.data[i] = 0xAB; dat.be[i] = 0xFF; }
    shm.layout()->g2q_dat.push(dat);

    tick(1);

    CHECK(p.rxdat_flitv.read(), "B.14: rxdat_flitv high for txn_id=0xFF");
    if (p.rxdat_flitv.read()) {
        DatFlit f = p.rxdat_flit.read();
        uint64_t tid  = bf_get(f, dat_f::TXNID_LO,  dat_f::TXNID_W);
        uint64_t resp = bf_get(f, dat_f::RESP_LO,    dat_f::RESP_W);
        uint64_t op   = bf_get(f, dat_f::OPCODE_LO,  dat_f::OPCODE_W);
        CHECK(tid  == 0xFF, "B.14: txn_id=0xFF (max) preserved in dat flit");
        CHECK(resp == 0x02, "B.14: resp=UC preserved alongside txn_id=0xFF");
        CHECK(op   == 0x04, "B.14: opcode=CompData preserved");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// sc_main
// ─────────────────────────────────────────────────────────────────────────────

int sc_main(int, char**)
{
    sc_set_time_resolution(1, SC_PS);
    printf("=== test_questa_top_ipc ===\n");

    // ── Part A: pack/unpack unit tests (no clock) ─────────────────────────────
    test_pack_resperr_rsp();
    test_pack_resperr_dat();
    test_pack_snp_ns();
    test_unpack_resperr_rsp();
    test_unpack_resperr_dat();

    // ── Part B: ALL modules must be created before sc_start() ────────────────
    // All questa_top_ipc instances are elaborated here, before any tick()
    // call (sc_start). SystemC does not allow inserting modules into the
    // hierarchy once simulation has started.

    sc_clock clk("clk", 1000, SC_PS);

    // Single shared instance for B.1–B.10
    std::string name = test_shm(100);
    ChiShmHandle shm_b(name, true);
    questa_top_ipc qt_b("qt_b", name, 1000);
    TopPorts ports_b;
    qt_b.clk(clk);
    bind(qt_b, ports_b);

    // B.1: reset cycle + credit count (first sc_start happens inside)
    test_b1_init_credits(clk, ports_b);
    // After B.1: rstn=true, init_credits_done, ready for B.2–B.10

    test_b2_rsp_resperr(clk, shm_b, ports_b);
    test_b3_dat_resperr(clk, shm_b, ports_b);
    test_b4_snp_ns(clk, shm_b, ports_b);
    test_b5_dat_be(clk, shm_b, ports_b);
    test_b6_req_flit_to_ipc(clk, shm_b, ports_b);
    test_b7_rsp_flit_to_ipc(clk, shm_b, ports_b);
    test_b8_dat_flit_to_ipc(clk, shm_b, ports_b);
    test_b9_questa_ps(clk, shm_b, ports_b);
    test_b10_reset(clk, shm_b, ports_b);
    test_b11_req_exp_comp_ack(clk, shm_b, ports_b);
    test_b12_rsp_fwd_state_q2g(clk, shm_b, ports_b);
    test_b13_dat_cc_id_q2g(clk, shm_b, ports_b);
    test_b14_rsp_fwd_state_g2q(clk, shm_b, ports_b);
    test_b15_dat_cc_id_g2q(clk, shm_b, ports_b);
    test_b11_multi_rsp(clk, shm_b, ports_b);
    test_b12_req_opcodes(clk, shm_b, ports_b);
    test_b13_snp_flags(clk, shm_b, ports_b);
    test_b14_dat_txnid_max(clk, shm_b, ports_b);

    printf("\n=== RESULTS: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
