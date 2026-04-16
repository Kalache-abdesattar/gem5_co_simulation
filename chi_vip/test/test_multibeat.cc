// test_multibeat.cc
//
// Tests for partial-write (byte-enable / WriteMask) and multi-beat DAT
// (DataID-based beat reassembly / splitting).
//
// All tests use latency=0 so sc_start() is only needed for elaboration;
// the test logic runs in sc_main with no SC_THREAD.
//
// CHI DataID byte-offset formula:
//   byte_offset = data_id * (cache_line_bytes / 4)
// For a 64-byte cache line: byte_offset = data_id * 16
//   128-bit bus (16 B/beat): DataID 0,1,2,3 → offsets  0,16,32,48
//   256-bit bus (32 B/beat): DataID 0,2     → offsets  0,32
//   512-bit bus (64 B/beat): DataID 0       → offset   0

#include <systemc>
#include <chi/chi_tlm.h>
#include <tlm_utils/simple_target_socket.h>
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

// ── InjectDat: drives rxdat_socket with configurable DataID and byte enables ──
SC_MODULE(InjectDat) {
    tlm_utils::simple_initiator_socket<InjectDat> sock;
    SC_CTOR(InjectDat) : sock("sock") {}

    // data_len: bytes in this beat (16, 32, or 64)
    // be:       byte-enable array of length be_len; nullptr = all bytes valid
    void send(chi::dat_optype_e op, unsigned txn_id, uint8_t data_id,
              const uint8_t* data, int data_len,
              const uint8_t* be = nullptr, int be_len = 0)
    {
        auto* ext = new chi::chi_data_extension();
        ext->cmn.set_txn_id(txn_id);
        ext->cmn.set_src_id(5);
        ext->dat.set_opcode(op);
        ext->dat.set_tgt_id(0);
        ext->dat.set_data_id(data_id);

        // Heap-allocate to avoid lifetime issues across b_transport call
        std::vector<uint8_t> buf(data_len, 0);
        if (data) memcpy(buf.data(), data, data_len);

        std::vector<uint8_t> be_vec;
        if (be && be_len > 0) be_vec.assign(be, be + be_len);

        tlm::tlm_generic_payload gp;
        gp.set_extension(ext);
        gp.set_address(0x2000);
        gp.set_command(tlm::TLM_WRITE_COMMAND);
        gp.set_data_ptr(buf.data());
        gp.set_data_length(static_cast<unsigned>(data_len));
        if (!be_vec.empty()) {
            gp.set_byte_enable_ptr(be_vec.data());
            gp.set_byte_enable_length(static_cast<unsigned>(be_vec.size()));
        }

        sc_time d = SC_ZERO_TIME;
        sock->b_transport(gp, d);
    }
};

// ── MultiCatchDat: captures every b_transport call on txdat_socket ───────────
SC_MODULE(MultiCatchDat) {
    tlm_utils::simple_target_socket<MultiCatchDat> sock;

    struct Beat {
        uint8_t data_id   = 0;
        int     data_len  = 0;
        uint8_t data[64]  = {};
    };
    std::vector<Beat> beats;

    SC_CTOR(MultiCatchDat) : sock("sock") {
        sock.register_b_transport(this, &MultiCatchDat::b_transport);
    }
    void b_transport(tlm::tlm_generic_payload& gp, sc_time&) {
        Beat b{};
        if (auto* e = gp.get_extension<chi::chi_data_extension>())
            b.data_id = e->dat.get_data_id();
        b.data_len = static_cast<int>(gp.get_data_length());
        if (gp.get_data_ptr() && b.data_len > 0 && b.data_len <= 64)
            memcpy(b.data, gp.get_data_ptr(), b.data_len);
        beats.push_back(b);
        gp.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void reset() { beats.clear(); }
};

// ── inject a CHIDataMsg directly into datIn (outbound test helper) ────────────
static void push_dat_to_datIn(CHI_VIP_Controller& ctrl,
                               CHIDataType dtype, Addr txn_id,
                               const uint8_t* bytes, RubySystem& rs)
{
    auto msg = std::make_shared<CHIDataMsg>(curTick(), 64, &rs);
    msg->settype(dtype);
    msg->settxnId(txn_id);
    MachineID src{MachineType_Directory, 0};
    msg->setresponder(src);
    MachineID tgt{MachineType_Directory, 0};
    msg->getDestination().add(tgt);
    if (bytes) {
        DataBlock blk;
        for (int i = 0; i < 64; i++) blk.setByte(i, bytes[i]);
        msg->setdataBlk(blk);
    }
    ctrl.getDatIn()->enqueue(msg, curTick(), 0, false, false);
}

// ─────────────────────────────────────────────────────────────────────────────
int sc_main(int, char**) {
    sc_set_time_resolution(1, SC_PS);
    printf("\n=== test_multibeat ===\n");

    gem5::ruby::RubySystem rs;
    rs.m_cache_line_size = 64;
    Network net;
    MachineID hnf_id{MachineType_Directory, 0};

    CHI_VIP_Controller ctrl("ctrl", hnf_id, &rs, &net);
    ctrl.downstream_ = hnf_id;

    InjectDat     inj("inj");
    MultiCatchDat catch_dat("catch_dat");

    inj.sock.bind(ctrl.rxdat_socket);
    ctrl.txdat_socket.bind(catch_dat.sock);
    ctrl.init();

    // ── A: single-beat backward compatibility (64-byte, DataID=0, no BE) ──────
    printf("\n[A] single-beat inbound (64-byte, DataID=0) — backward compat\n");
    {
        uint8_t tx[64] = {};
        tx[0]  = 0xAA;
        tx[63] = 0xBB;
        inj.send(chi::dat_optype_e::CopyBackWrData, /*txn_id=*/1,
                 /*data_id=*/0, tx, /*len=*/64);

        CHECK(ctrl.getDatOut()->isReady(curTick()),
              "datOut ready after single beat");
        const auto* msg = dynamic_cast<const CHIDataMsg*>(ctrl.getDatOut()->peek());
        CHECK(msg && msg->getdataBlk().getByte(0)  == 0xAA, "data[0]  == 0xAA");
        CHECK(msg && msg->getdataBlk().getByte(63) == 0xBB, "data[63] == 0xBB");
        CHECK(msg && msg->getbitMask().isSet(0),  "bitMask[0]  set (no BE)");
        CHECK(msg && msg->getbitMask().isSet(63), "bitMask[63] set (no BE)");
        ctrl.getDatOut()->dequeue(curTick());
    }

    // ── B: two-beat inbound (256-bit bus, DataID=0 then DataID=2) ────────────
    printf("\n[B] two-beat inbound (32-byte/beat, DataID 0→2)\n");
    {
        // Beat 0: bytes [0..31], DataID=0
        uint8_t beat0[32] = {};
        beat0[0]  = 0x11;
        beat0[31] = 0x22;
        inj.send(chi::dat_optype_e::CopyBackWrData, /*txn_id=*/2,
                 /*data_id=*/0, beat0, /*len=*/32);
        CHECK(!ctrl.getDatOut()->isReady(curTick()),
              "datOut NOT ready after first beat");

        // Beat 1: bytes [32..63], DataID=2 (offset = 2*16 = 32)
        uint8_t beat1[32] = {};
        beat1[0]  = 0x33;   // → cache-line byte 32
        beat1[31] = 0x44;   // → cache-line byte 63
        inj.send(chi::dat_optype_e::CopyBackWrData, /*txn_id=*/2,
                 /*data_id=*/2, beat1, /*len=*/32);
        CHECK(ctrl.getDatOut()->isReady(curTick()),
              "datOut ready after second beat");

        const auto* msg = dynamic_cast<const CHIDataMsg*>(ctrl.getDatOut()->peek());
        CHECK(msg && msg->getdataBlk().getByte(0)  == 0x11, "data[0]  == 0x11");
        CHECK(msg && msg->getdataBlk().getByte(31) == 0x22, "data[31] == 0x22");
        CHECK(msg && msg->getdataBlk().getByte(32) == 0x33, "data[32] == 0x33");
        CHECK(msg && msg->getdataBlk().getByte(63) == 0x44, "data[63] == 0x44");
        ctrl.getDatOut()->dequeue(curTick());
    }

    // ── C: partial write with byte enables (WriteMask) ───────────────────────
    printf("\n[C] partial write — byte enables mark only bytes [8..15]\n");
    {
        uint8_t tx[64] = {};
        for (int i = 8; i < 16; i++) tx[i] = static_cast<uint8_t>(0xC0 + i);

        // Build BE array: enabled for bytes 8..15 only
        uint8_t be[64] = {};
        for (int i = 8; i < 16; i++) be[i] = 0xFF;

        inj.send(chi::dat_optype_e::NonCopyBackWrData, /*txn_id=*/3,
                 /*data_id=*/0, tx, /*len=*/64, be, /*be_len=*/64);

        CHECK(ctrl.getDatOut()->isReady(curTick()), "datOut ready");
        const auto* msg = dynamic_cast<const CHIDataMsg*>(ctrl.getDatOut()->peek());
        CHECK(msg && msg->getdataBlk().getByte(8)  == 0xC8, "data[8]  == 0xC8");
        CHECK(msg && !msg->getbitMask().isSet(7),  "bitMask[7]  NOT set");
        CHECK(msg && msg->getbitMask().isSet(8),   "bitMask[8]  set");
        CHECK(msg && msg->getbitMask().isSet(15),  "bitMask[15] set");
        CHECK(msg && !msg->getbitMask().isSet(16), "bitMask[16] NOT set");
        ctrl.getDatOut()->dequeue(curTick());
    }

    // ── D: four-beat inbound (128-bit bus, DataID=0,1,2,3) ───────────────────
    printf("\n[D] four-beat inbound (16-byte/beat, DataID 0,1,2,3)\n");
    {
        // Use a distinct fill byte per beat to easily verify placement.
        const uint8_t fill[4] = {0xD0, 0xD1, 0xD2, 0xD3};
        for (int i = 0; i < 4; i++) {
            uint8_t beat[16];
            memset(beat, fill[i], 16);
            inj.send(chi::dat_optype_e::CopyBackWrData, /*txn_id=*/4,
                     static_cast<uint8_t>(i), beat, /*len=*/16);
        }
        CHECK(ctrl.getDatOut()->isReady(curTick()),
              "datOut ready after four beats");
        const auto* msg = dynamic_cast<const CHIDataMsg*>(ctrl.getDatOut()->peek());
        // offset = data_id * 16; all bytes in beat i land at cache-line [i*16..(i+1)*16-1]
        CHECK(msg && msg->getdataBlk().getByte(0)  == 0xD0, "data[0]  == 0xD0 (beat0)");
        CHECK(msg && msg->getdataBlk().getByte(15) == 0xD0, "data[15] == 0xD0 (beat0 last)");
        CHECK(msg && msg->getdataBlk().getByte(16) == 0xD1, "data[16] == 0xD1 (beat1 first)");
        CHECK(msg && msg->getdataBlk().getByte(31) == 0xD1, "data[31] == 0xD1 (beat1 last)");
        CHECK(msg && msg->getdataBlk().getByte(48) == 0xD3, "data[48] == 0xD3 (beat3 first)");
        CHECK(msg && msg->getdataBlk().getByte(63) == 0xD3, "data[63] == 0xD3 (beat3 last)");
        ctrl.getDatOut()->dequeue(curTick());
    }

    // ── E: outbound multi-beat (256-bit bus, dat_width_bytes_=32) ─────────────
    printf("\n[E] outbound split into 2 beats (dat_width_bytes_=32)\n");
    {
        ctrl.dat_width_bytes_ = 32;
        catch_dat.reset();

        uint8_t payload[64] = {};
        for (int i = 0; i < 64; i++) payload[i] = static_cast<uint8_t>(i);
        push_dat_to_datIn(ctrl, CHIDataType_CompData_UC, /*txn_id=*/5, payload, rs);
        ctrl.wakeup();

        CHECK(catch_dat.beats.size() == 2,
              "txdat_socket received exactly 2 beats");
        if (catch_dat.beats.size() == 2) {
            // Beat 0: DataID=0, bytes 0..31
            CHECK(catch_dat.beats[0].data_id   == 0,    "beat[0] DataID == 0");
            CHECK(catch_dat.beats[0].data_len  == 32,   "beat[0] length == 32");
            CHECK(catch_dat.beats[0].data[0]   == 0x00, "beat[0] data[0] == 0x00");
            CHECK(catch_dat.beats[0].data[31]  == 0x1F, "beat[0] data[31] == 0x1F");
            // Beat 1: DataID=2 (offset=32), bytes 32..63
            CHECK(catch_dat.beats[1].data_id   == 2,    "beat[1] DataID == 2");
            CHECK(catch_dat.beats[1].data_len  == 32,   "beat[1] length == 32");
            CHECK(catch_dat.beats[1].data[0]   == 0x20, "beat[1] data[0] == 0x20 (byte 32)");
            CHECK(catch_dat.beats[1].data[31]  == 0x3F, "beat[1] data[31] == 0x3F (byte 63)");
        }

        ctrl.dat_width_bytes_ = 64;  // restore default
    }

    // ── F: outbound four-beat (128-bit bus, dat_width_bytes_=16) ─────────────
    printf("\n[F] outbound split into 4 beats (dat_width_bytes_=16)\n");
    {
        ctrl.dat_width_bytes_ = 16;
        catch_dat.reset();

        uint8_t payload[64] = {};
        for (int i = 0; i < 64; i++) payload[i] = static_cast<uint8_t>(0x80 + i);
        push_dat_to_datIn(ctrl, CHIDataType_CompData_SC, /*txn_id=*/6, payload, rs);
        ctrl.wakeup();

        CHECK(catch_dat.beats.size() == 4,
              "txdat_socket received exactly 4 beats");
        if (catch_dat.beats.size() == 4) {
            CHECK(catch_dat.beats[0].data_id == 0, "beat[0] DataID == 0");
            CHECK(catch_dat.beats[1].data_id == 1, "beat[1] DataID == 1");
            CHECK(catch_dat.beats[2].data_id == 2, "beat[2] DataID == 2");
            CHECK(catch_dat.beats[3].data_id == 3, "beat[3] DataID == 3");
            CHECK(catch_dat.beats[0].data[0]  == 0x80, "beat[0] data[0] == 0x80");
            CHECK(catch_dat.beats[1].data[0]  == 0x90, "beat[1] data[0] == 0x90 (byte 16)");
            CHECK(catch_dat.beats[3].data[15] == 0xBF, "beat[3] data[15] == 0xBF (byte 63)");
        }

        ctrl.dat_width_bytes_ = 64;  // restore default
    }

    printf("\n=== RESULTS: %d passed, %d failed ===\n\n", g_pass, g_fail);
    sc_start(1, SC_NS);
    return g_fail != 0 ? 1 : 0;
}
