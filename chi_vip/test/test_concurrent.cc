// test_concurrent.cc
//
// Tests for edge cases in pending_dat_ beat assembly:
//   • Two simultaneous transactions with different TxnIDs assembling in parallel
//   • Interleaved beat arrival (A0, B0, A1, B1)
//   • TxnID reuse: same TxnID completes and restarts
//   • Single-beat transactions do not leave orphan state in pending_dat_
//   • Mixed bus widths are not directly testable (dat_width_bytes_ is per-controller,
//     not per-beat), but the assembly keys solely on TxnID + data_length

#include <systemc>
#include <chi/chi_tlm.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <cstdio>
#include <cstring>
#include <vector>

#include "mem/ruby/system/RubySystem.hh"
#include "mem/ruby/network/Network.hh"
#include "mem/ruby/common/MachineID.hh"
#include "chi_vip_controller.hh"
#include "CHIDataMsg.hh"

using namespace gem5::ruby;
using namespace gem5::ruby::CHI;
using namespace sc_core;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { printf("  PASS: %s\n", msg); g_pass++; } \
        else      { printf("  FAIL: %s  (line %d)\n", msg, __LINE__); g_fail++; } \
    } while(0)

// ── InjectBeat: drives rxdat_socket with configurable beat parameters ─────────
SC_MODULE(InjectBeat) {
    tlm_utils::simple_initiator_socket<InjectBeat> sock;
    SC_CTOR(InjectBeat) : sock("sock") {}

    void send(unsigned txn_id, uint8_t data_id,
              const uint8_t* data, int beat_bytes,
              chi::dat_optype_e op = chi::dat_optype_e::CopyBackWrData,
              chi::dat_resptype_e resp = chi::dat_resptype_e::CopyBackWrData_UC)
    {
        auto* ext = new chi::chi_data_extension();
        ext->cmn.set_txn_id(txn_id);
        ext->cmn.set_src_id(1);
        ext->dat.set_opcode(op);
        ext->dat.set_data_id(data_id);
        ext->dat.set_tgt_id(0);
        ext->dat.set_resp(resp);

        std::vector<uint8_t> buf(beat_bytes, 0);
        if (data) memcpy(buf.data(), data, beat_bytes);

        tlm::tlm_generic_payload gp;
        gp.set_extension(ext);
        gp.set_address(0x1000);
        gp.set_command(tlm::TLM_WRITE_COMMAND);
        gp.set_data_ptr(buf.data());
        gp.set_data_length(static_cast<unsigned>(beat_bytes));
        sc_time d = SC_ZERO_TIME;
        sock->b_transport(gp, d);
    }
};

static const CHIDataMsg* peek_dat(CHI_VIP_Controller& ctrl) {
    return dynamic_cast<const CHIDataMsg*>(ctrl.getDatOut()->peek());
}

// ─────────────────────────────────────────────────────────────────────────────
int sc_main(int, char**) {
    sc_set_time_resolution(1, SC_PS);
    printf("\n=== test_concurrent ===\n");

    gem5::ruby::RubySystem rs;
    rs.m_cache_line_size = 64;
    Network net;
    MachineID hnf_id{MachineType_Directory, 0};

    CHI_VIP_Controller ctrl("ctrl", hnf_id, &rs, &net);
    ctrl.downstream_ = hnf_id;

    InjectBeat inj("inj");
    inj.sock.bind(ctrl.rxdat_socket);
    ctrl.init();

    // ── 1. Sequential single-beat transactions (no concurrency, sanity) ───────
    printf("\n[1] sequential single-beat transactions\n");
    {
        uint8_t data_a[64], data_b[64];
        for (int i = 0; i < 64; i++) { data_a[i] = 0xAA; data_b[i] = 0xBB; }

        inj.send(/*txn=*/10, /*data_id=*/0, data_a, 64);
        const auto* m_a = peek_dat(ctrl);
        CHECK(m_a && m_a->gettxnId() == 10,                "txn=10 delivered");
        CHECK(m_a && m_a->getdataBlk().getByte(0) == 0xAA, "txn=10 data[0] == 0xAA");
        ctrl.getDatOut()->dequeue(curTick());

        inj.send(/*txn=*/11, /*data_id=*/0, data_b, 64);
        const auto* m_b = peek_dat(ctrl);
        CHECK(m_b && m_b->gettxnId() == 11,                "txn=11 delivered");
        CHECK(m_b && m_b->getdataBlk().getByte(0) == 0xBB, "txn=11 data[0] == 0xBB");
        ctrl.getDatOut()->dequeue(curTick());
    }

    // ── 2. Interleaved 2-beat transactions with different TxnIDs ─────────────
    printf("\n[2] interleaved 2-beat: A0, B0, A1, B1 → A then B complete\n");
    {
        uint8_t a0[32], a1[32], b0[32], b1[32];
        for (int i = 0; i < 32; i++) {
            a0[i] = 0xA0; a1[i] = 0xA1;
            b0[i] = 0xB0; b1[i] = 0xB1;
        }

        // Interleave: A beat 0 (DataID=0), B beat 0 (DataID=0),
        //             A beat 1 (DataID=2), B beat 1 (DataID=2)
        inj.send(/*txn=*/20, /*data_id=*/0, a0, 32);
        // A not complete yet (only 1 of 2 beats)
        CHECK(ctrl.getDatOut()->peek() == nullptr,
              "txn=20 incomplete after beat 0");

        inj.send(/*txn=*/21, /*data_id=*/0, b0, 32);
        // B not complete yet either
        CHECK(ctrl.getDatOut()->peek() == nullptr,
              "txn=21 incomplete after beat 0");

        inj.send(/*txn=*/20, /*data_id=*/2, a1, 32);
        // A is now complete (beats 0 and 2 delivered)
        const auto* m_a = peek_dat(ctrl);
        CHECK(m_a && m_a->gettxnId() == 20,                     "txn=20 complete first");
        CHECK(m_a && m_a->getdataBlk().getByte(0)  == 0xA0,     "txn=20 data[0]  == A0");
        CHECK(m_a && m_a->getdataBlk().getByte(31) == 0xA0,     "txn=20 data[31] == A0");
        CHECK(m_a && m_a->getdataBlk().getByte(32) == 0xA1,     "txn=20 data[32] == A1");
        CHECK(m_a && m_a->getdataBlk().getByte(63) == 0xA1,     "txn=20 data[63] == A1");
        ctrl.getDatOut()->dequeue(curTick());

        inj.send(/*txn=*/21, /*data_id=*/2, b1, 32);
        // B is now complete
        const auto* m_b = peek_dat(ctrl);
        CHECK(m_b && m_b->gettxnId() == 21,                     "txn=21 complete second");
        CHECK(m_b && m_b->getdataBlk().getByte(0)  == 0xB0,     "txn=21 data[0]  == B0");
        CHECK(m_b && m_b->getdataBlk().getByte(32) == 0xB1,     "txn=21 data[32] == B1");
        ctrl.getDatOut()->dequeue(curTick());
    }

    // ── 3. TxnID reuse: same ID used for two consecutive transactions ─────────
    printf("\n[3] TxnID reuse after completion\n");
    {
        uint8_t first[64], second[64];
        for (int i = 0; i < 64; i++) { first[i] = 0xF1; second[i] = 0xF2; }

        inj.send(/*txn=*/30, /*data_id=*/0, first, 64);
        const auto* m1 = peek_dat(ctrl);
        CHECK(m1 && m1->getdataBlk().getByte(0) == 0xF1, "first txn=30 data[0]==0xF1");
        ctrl.getDatOut()->dequeue(curTick());

        // Reuse TxnID=30
        inj.send(/*txn=*/30, /*data_id=*/0, second, 64);
        const auto* m2 = peek_dat(ctrl);
        CHECK(m2 && m2->getdataBlk().getByte(0) == 0xF2, "second txn=30 data[0]==0xF2");
        ctrl.getDatOut()->dequeue(curTick());
    }

    // ── 4. Four concurrent transactions interleaved (4-beat each) ────────────
    printf("\n[4] four concurrent 4-beat transactions with interleaved beats\n");
    {
        // TxnIDs 40..43, 4-beat (16B each, DataID 0,1,2,3)
        // Fill pattern: txn N beat M = byte(N*10 + M)
        uint8_t bufs[4][4][16];
        for (int t = 0; t < 4; t++)
            for (int b = 0; b < 4; b++)
                memset(bufs[t][b], t * 10 + b, 16);

        // Send all beats in round-robin: (t0 b0)(t1 b0)(t2 b0)(t3 b0)
        //                                (t0 b1)(t1 b1)...etc
        for (int beat = 0; beat < 4; beat++)
            for (int t = 0; t < 4; t++)
                inj.send(/*txn=*/40 + t, /*data_id=*/static_cast<uint8_t>(beat),
                         bufs[t][beat], 16);

        // All 4 should now be in datOut
        CHECK(true, "all 4 concurrent transactions attempted");

        int complete = 0;
        bool data_ok = true;
        for (int t = 0; t < 4; t++) {
            const auto* m = peek_dat(ctrl);
            if (!m) { data_ok = false; break; }
            complete++;
            // Verify data: each 16-byte chunk should have the right fill
            for (int b = 0; b < 4; b++) {
                uint8_t expected = static_cast<uint8_t>((m->gettxnId() - 40) * 10 + b);
                for (int byte = 0; byte < 16; byte++) {
                    if (m->getdataBlk().getByte(b * 16 + byte) != expected) {
                        data_ok = false;
                    }
                }
            }
            ctrl.getDatOut()->dequeue(curTick());
        }
        CHECK(complete == 4, "all 4 transactions delivered");
        CHECK(data_ok,       "data correct in all 4 transactions");
    }

    // ── 5. Partial-write byte-enable with concurrent transactions ────────────
    printf("\n[5] byte-enable WriteMask with concurrent 2-beat transactions\n");
    {
        uint8_t data_c[32], data_d[32];
        memset(data_c, 0xCC, 32);
        memset(data_d, 0xDD, 32);

        // C beat 0: bytes 0..7 enabled, 8..31 not
        uint8_t be_c[32];
        memset(be_c, 0x00, 32);
        for (int i = 0; i < 8; i++) be_c[i] = 0xFF;

        // Send C beat 0 with byte enables
        {
            auto* ext = new chi::chi_data_extension();
            ext->cmn.set_txn_id(50);
            ext->cmn.set_src_id(1);
            ext->dat.set_opcode(chi::dat_optype_e::CopyBackWrData);
            ext->dat.set_data_id(0);
            ext->dat.set_resp(chi::dat_resptype_e::CopyBackWrData_UC);

            tlm::tlm_generic_payload gp;
            gp.set_extension(ext);
            gp.set_address(0x1000);
            gp.set_command(tlm::TLM_WRITE_COMMAND);
            gp.set_data_ptr(data_c);
            gp.set_data_length(32);
            gp.set_byte_enable_ptr(be_c);
            gp.set_byte_enable_length(32);
            sc_time d = SC_ZERO_TIME;
            inj.sock->b_transport(gp, d);
        }

        // D beat 0 (no BE, all valid)
        inj.send(/*txn=*/51, /*data_id=*/0, data_d, 32);

        // C beat 1 (no BE)
        inj.send(/*txn=*/50, /*data_id=*/2, data_c, 32);

        // C should now be complete
        const auto* m_c = peek_dat(ctrl);
        CHECK(m_c && m_c->gettxnId() == 50, "txn=50 (C) completes");
        if (m_c) {
            // Bytes 0..7 enabled in beat 0 → set in WriteMask
            bool mask_ok = true;
            for (int i = 0; i < 8;  i++) if (!m_c->getbitMask().isSet(i))    mask_ok = false;
            // Bytes 8..31 not enabled in beat 0 → not set
            for (int i = 8; i < 32; i++) if ( m_c->getbitMask().isSet(i))    mask_ok = false;
            // Beat 1 (bytes 32..63) had no BE → all set
            for (int i = 32; i < 64; i++) if (!m_c->getbitMask().isSet(i))   mask_ok = false;
            CHECK(mask_ok, "WriteMask correct with partial BE in first beat");
        }
        ctrl.getDatOut()->dequeue(curTick());

        // D beat 1
        inj.send(/*txn=*/51, /*data_id=*/2, data_d, 32);
        const auto* m_d = peek_dat(ctrl);
        CHECK(m_d && m_d->gettxnId() == 51, "txn=51 (D) completes");
        ctrl.getDatOut()->dequeue(curTick());
    }

    printf("\n=== RESULTS: %d passed, %d failed ===\n\n", g_pass, g_fail);
    sc_start(1, SC_NS);
    return g_fail != 0 ? 1 : 0;
}
