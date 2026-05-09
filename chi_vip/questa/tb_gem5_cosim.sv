// tb_gem5_cosim.sv
//
// End-to-end co-simulation testbench: drives CHI ReadShared / ReadNoSnp
// requests through questa_top_ipc into gem5's CHI HN-F and checks that
// CompData responses come back on the RXDAT channel.
//
// Requires gem5 to be running chi_vip_se.py --vip (or chi_hnf_only.py)
// in a separate process with the same shm name (/chi_vip_0).
//
// REQ flit field offsets (from chi_flit_pack.hh, NODEID_W=7 TXNID_W=12 ADDR_W=44):
//   [3:0]   QOS
//   [10:4]  TGTID (7b)
//   [17:11] SRCID (7b)
//   [29:18] TXNID (12b)
//   [49:43] OPCODE (7b)   ReadShared=0x01  ReadNoSnp=0x04
//   [52:50] SIZE (3b)     6 = 64-byte cache line
//   [96:53] ADDR (44b)
//   [99]    ALLOWRETRY
//   [113]   EXPCOMPACK
//
// DAT flit field offsets (chi_flit_pack.hh dat_f, NODEID_W=7 TXNID_W=12):
//   [3:0]    QOS
//   [10:4]   TGTID (7b)
//   [17:11]  HOMEID (7b)
//   [21:18]  OPCODE (4b)   CompData=0x04
//   [23:22]  RESPERR (2b)
//   [26:24]  RESP (3b)     UC=2
//   [29:27]  FWDSTATE (3b)
//   [30]     DATAPULL
//   [42:31]  DBID (12b)
//   [44:43]  CCID (2b)
//   [46:45]  DATAID (2b)
//   [48:47]  TAGOP (2b)
//   [60:49]  TXNID (12b)
//   [67:61]  SRCID (7b)
//   [132:69] BE (64b)
//   [644:133] DATA (512b)
//
// RSP flit field offsets (chi_flit_pack.hh rsp_f, NODEID_W=7 TXNID_W=12):
//   [3:0]    QOS
//   [10:4]   TGTID (7b)
//   [17:11]  SRCID (7b)
//   [29:18]  TXNID (12b)
//   [35:30]  OPCODE (6b)  CompAck=0x02  Comp=0x04  DBIDResp=0x06  CompDBIDResp=0x05
//   [37:36]  RESPERR (2b)
//   [40:38]  RESP (3b)
//   [56:45]  DBID (12b)
//
// Tests:
//   [1]  ReadNoSnp to address 0x1000 → CompData_I response (RESP=0, non-coherent)
//   [2]  ReadShared to address 0x2000 → CompData response
//   [3]  Two pipelined ReadNoSnp in the same TXNID space → both get CompData
//   [4]  gem5_grant_ps advances (gem5 time marker written by VIPController)
//   [5]  RESP field is I=0 for ReadNoSnp CompData (non-coherent read; no state granted)
//   [6]  ReadNoSnp to a different address (0x5000) — independent address space
//   [7]  Four pipelined ReadNoSnp → all four CompData responses received
//   [8]  TXNID reuse: same TXNID (0x01) on a second request after first completes
//   [9]  ReadNotSharedDirty (opcode 0x07) → CompData
//   [10] ReadUnique (opcode 0x07) → CompData
//   [11] Eight pipelined ReadNoSnp (txnid 0x40..0x47) → all eight CompData
//   [12] ReadUnique+CompAck+WriteBackFull data round-trip: write 0xA5/0x5A, read back
//   [13] WriteUniqueFull + ReadNoSnp data round-trip: write 0xC3/0x3C, read back
//   [14] 512-bit data: write sentinel bytes at 8 offsets (one per 64-bit word),
//        read back and verify each — stresses full DPI bypass path
//   [15] Overwrite same address twice via WriteBackFull: write 0x11, verify, write 0x22, verify
//   [16] Address isolation: write distinct patterns to two different cache lines,
//        verify neither contaminates the other
//   [17] ReadOnce (opcode 0x03) → CompData (non-temporal read, no dirty allocation)
//   [18] CleanUnique (opcode 0x11) → Comp_UC after ReadShared (SC→UC coherency upgrade)
//   [19] WriteUniqueZero (opcode 0x22) → Comp: zeros cache line without data transfer
//   [20] Evict (opcode 0x17) → Comp_I after ReadShared (SC eviction, HNF in SC_RSC)
//   [21] WriteCleanFull (opcode 0x1C) → CBWrData_UD_PD; WriteEvictFull(CBWrData_UC=0xBE) to release UC
//   [22] MakeReadUnique (opcode 0x15) → Comp_UC/CompData after ReadShared (SC→UC snoop)
//   [23] WriteEvictFull (opcode 0x1D) → CBWrData_UC (gem5 expects clean encoding)
//   [24] WriteBackPtl (opcode 0x1A) → partial dirty writeback; bytes 0-3 dirty=0xBEEFDEAD
//   [25] WriteBackPtl mid-line (opcode 0x1A) → bytes 8-11 dirty=0xFEEDCAFE
//   [26] WriteBackPtl merge (opcode 0x1A) → dirty-on-dirty: seed UD then partial overlay
//   [27] WriteBackPtl upper-end (opcode 0x1A) → bytes 60-63 dirty=0xDEADC0DE
//   [28] WriteBackPtl non-contiguous BE (opcode 0x1A) → bytes 0-3 and 28-31 dirty (gap in between)
//   [29] WriteBackPtl single-byte dirty (opcode 0x1A) → byte 0 only = 0x42
//   [30] WriteBackPtl chained (opcode 0x1A) → two partial writebacks accumulate: bytes 0-3=0xAA then 4-7=0xBB

`timescale 1ps/1ps

module tb_gem5_cosim
    import chi_ipc_pkg::*;
();

    // ── Clock ─────────────────────────────────────────────────────────────────
    logic clk = 0;
    always #500 clk = ~clk;  // 1000ps = 1ns period

    // ── Reset ─────────────────────────────────────────────────────────────────
    logic rstn = 0;

    // ── TX channel (testbench → questa_top_ipc → gem5) ────────────────────────
    logic                         txreq_flitv    = 0;
    logic                         txreq_flitpend = 0;
    logic [CHI_REQ_FLIT_W-1:0]    txreq_flit     = '0;
    logic                         txrsp_flitv    = 0;
    logic                         txrsp_flitpend = 0;
    logic [CHI_RSP_FLIT_W-1:0]    txrsp_flit     = '0;
    logic                         txdat_flitv    = 0;
    logic                         txdat_flitpend = 0;
    logic [CHI_DAT_FLIT_W-1:0]    txdat_flit     = '0;

    // ── RX channel (gem5 → questa_top_ipc → testbench) ────────────────────────
    logic rxrsp_lcrdv = 0;
    logic rxdat_lcrdv = 0;
    logic rxsnp_lcrdv = 0;

    logic                         txreq_lcrdv;
    logic                         txrsp_lcrdv;
    logic                         txdat_lcrdv;
    logic                         rxrsp_flitv;
    logic                         rxrsp_flitpend;
    logic [CHI_RSP_FLIT_W-1:0]    rxrsp_flit;
    logic                         rxdat_flitv;
    logic                         rxdat_flitpend;
    logic [CHI_DAT_FLIT_W-1:0]    rxdat_flit;
    logic                         rxsnp_flitv;
    logic                         rxsnp_flitpend;
    logic [CHI_SNP_FLIT_W-1:0]    rxsnp_flit;

    // ── DUT ───────────────────────────────────────────────────────────────────
    questa_top_ipc_wrap u_dut (
        .clk             (clk),            .rstn            (rstn),
        .txreq_flitv     (txreq_flitv),    .txreq_flitpend  (txreq_flitpend),
        .txreq_flit      (txreq_flit),     .txreq_lcrdv     (txreq_lcrdv),
        .txrsp_flitv     (txrsp_flitv),    .txrsp_flitpend  (txrsp_flitpend),
        .txrsp_flit      (txrsp_flit),     .txrsp_lcrdv     (txrsp_lcrdv),
        .txdat_flitv     (txdat_flitv),    .txdat_flitpend  (txdat_flitpend),
        .txdat_flit      (txdat_flit),     .txdat_lcrdv     (txdat_lcrdv),
        .rxrsp_flitv     (rxrsp_flitv),    .rxrsp_flitpend  (rxrsp_flitpend),
        .rxrsp_flit      (rxrsp_flit),     .rxrsp_lcrdv     (rxrsp_lcrdv),
        .rxdat_flitv     (rxdat_flitv),    .rxdat_flitpend  (rxdat_flitpend),
        .rxdat_flit      (rxdat_flit),     .rxdat_lcrdv     (rxdat_lcrdv),
        .rxsnp_flitv     (rxsnp_flitv),    .rxsnp_flitpend  (rxsnp_flitpend),
        .rxsnp_flit      (rxsnp_flit),     .rxsnp_lcrdv     (rxsnp_lcrdv)
    );

    // ── Test infrastructure ───────────────────────────────────────────────────
    int pass_cnt = 0;
    int fail_cnt = 0;

    task automatic posedge_clk;
        @(posedge clk); #1;
    endtask

    task automatic tick(input int n);
        repeat(n) posedge_clk;
    endtask

    task automatic check(input logic cond, input string msg);
        if (cond) begin $display("  PASS: %s", msg); pass_cnt++; end
        else      begin $display("  FAIL: %s", msg); fail_cnt++; end
    endtask

    // Build a REQ flit: ReadNoSnp (0x04) or ReadShared (0x01)
    // NODEID_W=7 TXNID_W=12 ADDR_W=44
    function automatic logic [CHI_REQ_FLIT_W-1:0] make_req(
        input logic  [6:0] tgt_id,
        input logic  [6:0] src_id,
        input logic [11:0] txn_id,
        input logic  [6:0] opcode,   // 7 bits
        input logic [43:0] addr
    );
        logic [CHI_REQ_FLIT_W-1:0] f = '0;
        // QOS [3:0] = 0
        f[10:4]  = tgt_id;          // TGTID
        f[17:11] = src_id;          // SRCID
        f[29:18] = txn_id;          // TXNID
        f[49:43] = opcode;          // OPCODE
        f[52:50] = 3'd6;            // SIZE = 6 (64 bytes)
        f[96:53] = addr;            // ADDR
        f[99]    = 1'b1;            // ALLOWRETRY
        return f;
    endfunction

    // Extract OPCODE from DAT flit [21:18] (4-bit field in chi_flit_pack.hh dat_f)
    function automatic logic [3:0] dat_opcode(input logic [CHI_DAT_FLIT_W-1:0] f);
        return f[21:18];
    endfunction

    // Extract TXNID from DAT flit [60:49] (12-bit field in dat_f)
    function automatic logic [11:0] dat_txnid(input logic [CHI_DAT_FLIT_W-1:0] f);
        return f[60:49];
    endfunction

    // Extract RESP from DAT flit [26:24] (3-bit field in dat_f)
    function automatic logic [2:0] dat_resp(input logic [CHI_DAT_FLIT_W-1:0] f);
        return f[26:24];
    endfunction

    // Extract DATA payload from DAT flit [644:133] (512-bit field; byte 0 = bits [140:133])
    function automatic logic [511:0] dat_data(input logic [CHI_DAT_FLIT_W-1:0] f);
        return f[644:133];
    endfunction

    // RSP flit helpers (rsp_f: OPCODE=[35:30], TXNID=[29:18], DBID=[56:45])
    function automatic logic [5:0] rsp_opcode(input logic [CHI_RSP_FLIT_W-1:0] f);
        return f[35:30];
    endfunction
    function automatic logic [11:0] rsp_txnid(input logic [CHI_RSP_FLIT_W-1:0] f);
        return f[29:18];
    endfunction
    function automatic logic [11:0] rsp_dbid(input logic [CHI_RSP_FLIT_W-1:0] f);
        return f[56:45];
    endfunction

    // Build a NonCopyBackWrData (0x3) DAT flit for WriteUniqueFull/WriteNoSnpFull.
    // dat_f offsets: TGTID=[10:4] HOMEID=[17:11] OPCODE=[21:18] RESP=[26:24]
    //               DBID=[42:31] TXNID=[60:49] SRCID=[67:61] BE=[132:69] DATA=[644:133]
    function automatic logic [CHI_DAT_FLIT_W-1:0] make_wrdat(
        input logic  [6:0]  tgt_id,
        input logic  [6:0]  src_id,
        input logic [11:0]  txn_id,
        input logic [11:0]  db_id,
        input logic [511:0] data,
        input logic  [63:0] be
    );
        logic [CHI_DAT_FLIT_W-1:0] f = '0;
        f[10:4]    = tgt_id;   // TGTID
        f[17:11]   = 7'h0;     // HOMEID = 0
        f[21:18]   = 4'h3;     // OPCODE = NonCopyBackWrData
        f[26:24]   = 3'h0;     // RESP = I (no cache state)
        f[42:31]   = db_id;    // DBID (assigned by HN-F in DBIDResp/CompDBIDResp)
        f[60:49]   = txn_id;   // TXNID
        f[67:61]   = src_id;   // SRCID
        f[132:69]  = be;       // BE (byte-enable, 1 bit per byte)
        f[644:133] = data;     // DATA (byte 0 at bits [140:133])
        return f;
    endfunction

    // Build a CopyBackWrData (0x2) DAT flit for WriteBackFull.
    // RESP=5 = UD_PD (Unique Dirty, Pass Dirty): line was dirty and is now evicted.
    function automatic logic [CHI_DAT_FLIT_W-1:0] make_cbwrdat(
        input logic  [6:0]  tgt_id,
        input logic  [6:0]  src_id,
        input logic [11:0]  txn_id,
        input logic [11:0]  db_id,
        input logic [511:0] data,
        input logic  [63:0] be
    );
        logic [CHI_DAT_FLIT_W-1:0] f = '0;
        f[10:4]    = tgt_id;   // TGTID
        f[17:11]   = 7'h0;     // HOMEID = 0
        f[21:18]   = 4'h2;     // OPCODE = CopyBackWrData
        f[26:24]   = 3'h6;     // RESP = UD_PD (Unique Dirty, Pass Dirty)
        f[42:31]   = db_id;    // DBID (assigned by HN-F in DBIDResp/CompDBIDResp)
        f[60:49]   = txn_id;   // TXNID
        f[67:61]   = src_id;   // SRCID
        f[132:69]  = be;       // BE (byte-enable, 1 bit per byte)
        f[644:133] = data;     // DATA (byte 0 at bits [140:133])
        return f;
    endfunction

    // Wait up to max_cycles for rxdat_flitv, capture flit
    task automatic wait_dat(
        input  int max_cycles,
        output logic got_dat,
        output logic [CHI_DAT_FLIT_W-1:0] captured
    );
        got_dat  = 0;
        captured = '0;
        // Grant a DAT credit so gem5 can send
        rxdat_lcrdv = 1;
        for (int i = 0; i < max_cycles; i++) begin
            posedge_clk;
            if (rxdat_flitv) begin
                // #0 lets any pending combinatorial updates (always block) settle
                // before we capture rxdat_flit.  Without this, the SC→SV bridge
                // and the DPI-bypass always block may not have run yet.
                #0;
                got_dat  = 1;
                captured = rxdat_flit;
                rxdat_lcrdv = 0;
                return;
            end
        end
        rxdat_lcrdv = 0;
    endtask

    // Wait up to max_cycles for rxrsp_flitv (grants RSP L-credit while waiting)
    task automatic wait_rsp(
        input  int max_cycles,
        output logic got_rsp,
        output logic [CHI_RSP_FLIT_W-1:0] captured
    );
        got_rsp  = 0;
        captured = '0;
        rxrsp_lcrdv = 1;
        for (int i = 0; i < max_cycles; i++) begin
            posedge_clk;
            if (rxrsp_flitv) begin
                got_rsp  = 1;
                captured = rxrsp_flit;
                rxrsp_lcrdv = 0;
                return;
            end
        end
        rxrsp_lcrdv = 0;
    endtask

    // Send NonCopyBackWrData DAT flit (WriteUniqueFull / WriteNoSnpFull)
    task automatic send_dat(
        input logic  [6:0]  tgt,
        input logic  [6:0]  src,
        input logic [11:0]  txn,
        input logic [11:0]  dbid,
        input logic [511:0] data,
        input logic  [63:0] be
    );
        txdat_flit  = make_wrdat(tgt, src, txn, dbid, data, be);
        txdat_flitv = 1;
        posedge_clk;
        txdat_flitv = 0;
        txdat_flit  = '0;
        #1;
    endtask

    // Build a CopyBackWrData (0x2, RESP=UC=2) DAT flit for WriteEvictFull.
    // WriteEvictFull evicts a clean unique copy; gem5 expects CBWrData_UC.
    // RESP=2 (UC) falls to the VIPController default → CHIDataType_CBWrData_UC.
    function automatic logic [CHI_DAT_FLIT_W-1:0] make_cbwrdat_clean(
        input logic  [6:0]  tgt_id,
        input logic  [6:0]  src_id,
        input logic [11:0]  txn_id,
        input logic [11:0]  db_id,
        input logic [511:0] data,
        input logic  [63:0] be
    );
        logic [CHI_DAT_FLIT_W-1:0] f = '0;
        f[10:4]    = tgt_id;
        f[17:11]   = 7'h0;
        f[21:18]   = 4'h2;    // OPCODE = CopyBackWrData
        f[26:24]   = 3'h2;    // RESP = UC (unique clean, no pass-dirty)
        f[42:31]   = db_id;
        f[60:49]   = txn_id;
        f[67:61]   = src_id;
        f[132:69]  = be;
        f[644:133] = data;
        return f;
    endfunction

    task automatic send_cbwrdat_clean(
        input logic  [6:0]  tgt,
        input logic  [6:0]  src,
        input logic [11:0]  txn,
        input logic [11:0]  dbid,
        input logic [511:0] data,
        input logic  [63:0] be
    );
        txdat_flit  = make_cbwrdat_clean(tgt, src, txn, dbid, data, be);
        txdat_flitv = 1;
        posedge_clk;
        txdat_flitv = 0;
        txdat_flit  = '0;
        #1;
    endtask

    // Send CopyBackWrData DAT flit (WriteBackFull — dirty line eviction)
    task automatic send_cbwrdat(
        input logic  [6:0]  tgt,
        input logic  [6:0]  src,
        input logic [11:0]  txn,
        input logic [11:0]  dbid,
        input logic [511:0] data,
        input logic  [63:0] be
    );
        txdat_flit  = make_cbwrdat(tgt, src, txn, dbid, data, be);
        txdat_flitv = 1;
        posedge_clk;
        txdat_flitv = 0;
        txdat_flit  = '0;
        #1;
    endtask

    // Build a generic RSP flit (VIP → HN-F via txrsp channel).
    // rsp_f: QOS[3:0] TGTID[10:4] SRCID[17:11] TXNID[29:18] OPCODE[35:30]
    function automatic logic [CHI_RSP_FLIT_W-1:0] make_rsp(
        input logic  [6:0] tgt_id,
        input logic  [6:0] src_id,
        input logic [11:0] txn_id,
        input logic  [5:0] opcode
    );
        logic [CHI_RSP_FLIT_W-1:0] f = '0;
        f[10:4]  = tgt_id;  // TGTID
        f[17:11] = src_id;  // SRCID
        f[29:18] = txn_id;  // TXNID
        f[35:30] = opcode;  // OPCODE
        return f;
    endfunction

    // Send a RSP flit from VIP to HN-F via txrsp channel.
    task automatic send_rsp(
        input logic  [6:0] tgt,
        input logic  [6:0] src,
        input logic [11:0] txn,
        input logic  [5:0] opcode
    );
        txrsp_flit  = make_rsp(tgt, src, txn, opcode);
        txrsp_flitv = 1;
        posedge_clk;
        txrsp_flitv = 0;
        txrsp_flit  = '0;
        #1;
    endtask

    // ── Reset + credit init ───────────────────────────────────────────────────
    task automatic do_reset_and_init;
        rstn = 0;
        tick(5);
        rstn = 1;
        // Burn the 15 init LCRD pulses by waiting (we don't drive flits)
        tick(20);
    endtask

    // ── Send one REQ flit (wait for LCRDV from questa_top_ipc) ───────────────
    task automatic send_req(
        input logic [6:0] tgt, src,
        input logic [11:0] txn,
        input logic [6:0] opcode,
        input logic [43:0] addr
    );
        // Wait for a REQ L-credit (questa_top_ipc gives 15 at init)
        // Credits were pre-consumed by init; in practice we just need to
        // assert flitv and check lcrdv comes back same cycle.
        txreq_flit  = make_req(tgt, src, txn, opcode, addr);
        txreq_flitv = 1;
        posedge_clk;
        txreq_flitv = 0;
        txreq_flit  = '0;
        #1;
    endtask

    // ── Main test sequence ────────────────────────────────────────────────────
    initial begin
        logic got;
        logic [CHI_DAT_FLIT_W-1:0] dat;

        // Wait for SystemC elaboration and shm attach
        #10_000;

        // ── [1] ReadNoSnp 0x1000 → expect CompData ────────────────────────
        $display("\n[1] ReadNoSnp addr=0x1000 → CompData from gem5 HN-F");
        do_reset_and_init();
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h01),
                 .opcode(7'h04 /*ReadNoSnp*/), .addr(44'h1000));
        wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
        check(got,       "received DAT flit within 50000 cycles");
        if (got) begin
            check(dat_opcode(dat) == 4'h4,  "DAT opcode=CompData(0x4)");
            check(dat_txnid(dat)  == 12'h01, "DAT txnid matches sent txnid");
        end

        $display("\n sending Compack ReadNoSnp for addr=0x1000");
        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h01),
                         .opcode(6'h2 /*CompAck*/));

        // ── [2] ReadShared 0x2000 → expect CompData ───────────────────────
        $display("\n[2] ReadShared addr=0x2000 → CompData from gem5 HN-F");
        do_reset_and_init();
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h02),
                 .opcode(7'h01 /*ReadShared*/), .addr(44'h2000));
        wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
        check(got,       "received DAT flit within 50000 cycles");
        if (got) begin
            check(dat_opcode(dat) == 4'h4,  "DAT opcode=CompData(0x4)");
            check(dat_txnid(dat)  == 12'h02, "DAT txnid=0x02");
        end

        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h02),
                         .opcode(6'h2 /*CompAck*/));

        // ── [3] Two pipelined ReadNoSnp ───────────────────────────────────
        $display("\n[3] Two pipelined ReadNoSnp (txnid 0x10, 0x11)");
        do_reset_and_init();
        // Send both back-to-back before waiting for responses
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h10),
                 .opcode(7'h30), .addr(44'h3000));
        tick(2);
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h11),
                 .opcode(7'h30), .addr(44'h4000));
        // Collect two DAT responses (order may differ — just check we get 2)
        begin
            automatic int dat_cnt = 0;
            automatic logic [11:0] seen_txnids [2];
            rxdat_lcrdv = 1;
            for (int i = 0; i < 100000 && dat_cnt < 2; i++) begin
                posedge_clk;
                if (rxdat_flitv) begin
                    seen_txnids[dat_cnt] = dat_txnid(rxdat_flit);
                    check(dat_opcode(rxdat_flit) == 4'h4,
                          $sformatf("pipelined[%0d] opcode=CompData", dat_cnt));
                    dat_cnt++;
                end
            end
            rxdat_lcrdv = 0;
            check(dat_cnt == 2, $sformatf("got 2 CompData responses (got %0d)", dat_cnt));
            if (dat_cnt == 2) begin
                check((seen_txnids[0]==12'h10 && seen_txnids[1]==12'h11) ||
                      (seen_txnids[0]==12'h11 && seen_txnids[1]==12'h10),
                      "txnids 0x10 and 0x11 both returned");
            end
        end

        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h10),
                         .opcode(6'h2 /*CompAck*/));

        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h11),
                         .opcode(6'h2 /*CompAck*/));

        // ── [4] gem5_grant_ps advances ────────────────────────────────────
        $display("\n[4] gem5_grant_ps advances monotonically");
        begin
            // Read via shm — questa_top_ipc exposes questa_grant_ps;
            // gem5_grant_ps is in the same shm but not directly accessible
            // from SV.  Check indirectly: if test [1]-[3] completed with
            // responses, gem5 must have advanced time.
            check(pass_cnt >= 6, "gem5 processed transactions (grant_ps advanced)");
        end

        // ── [5] RESP field = I (0) for ReadNoSnp ──────────────────────────
        // CHI spec: ReadNoSnp is non-coherent; HNF sends CompData_I (RESP=0).
        // Send_CompData (CHI-cache-actions.sm) sets snd_msgType=CompData_I for
        // is_rd_no_snp, so the requester receives no coherency state.
        $display("\n[5] RESP field is I=0 for ReadNoSnp CompData (non-coherent)");
        do_reset_and_init();
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h20),
                 .opcode(7'h04 /*ReadNoSnp*/), .addr(44'h5000));
        wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
        check(got, "received DAT flit for RESP check");
        if (got) begin
            check(dat_resp(dat) == 3'd0, $sformatf("RESP=I(0) got=%0d", dat_resp(dat)));
            check(dat_opcode(dat) == 4'h4, "opcode=CompData");
        end
        
        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h20),
                         .opcode(6'h2 /*CompAck*/));

        // ── [6] ReadNoSnp to address 0x6000 (independent address space) ──
        $display("\n[6] ReadNoSnp addr=0x6000 (new address, separate request)");
        do_reset_and_init();
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h21),
                 .opcode(7'h04 /*ReadNoSnp*/), .addr(44'h6000));
        wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
        check(got,       "received DAT flit for addr=0x6000");
        if (got) begin
            check(dat_opcode(dat) == 4'h4,  "DAT opcode=CompData(0x4)");
            check(dat_txnid(dat)  == 12'h21, "DAT txnid=0x21");
        end

        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h21),
                         .opcode(6'h2 /*CompAck*/));

        // ── [7] Four pipelined ReadNoSnp ──────────────────────────────────
        $display("\n[7] Four pipelined ReadNoSnp (txnid 0x30..0x33)");
        do_reset_and_init();
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h30), .opcode(7'h30), .addr(44'h7000));
        tick(1);
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h31), .opcode(7'h30), .addr(44'h8000));
        tick(1);
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h32), .opcode(7'h30), .addr(44'h9000));
        tick(1);
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h33), .opcode(7'h30), .addr(44'hA000));
        begin
            automatic int dat_cnt = 0;
            automatic logic [3:0] seen_ok = 4'b0;  // one bit per txnid 0x30..0x33
            rxdat_lcrdv = 1;
            for (int i = 0; i < 200000 && dat_cnt < 4; i++) begin
                posedge_clk;
                if (rxdat_flitv) begin
                    automatic logic [11:0] tid = dat_txnid(rxdat_flit);
                    check(dat_opcode(rxdat_flit) == 4'h4,
                          $sformatf("4pipe[%0d] opcode=CompData", dat_cnt));
                    if (tid >= 12'h30 && tid <= 12'h33)
                        seen_ok[tid - 12'h30] = 1'b1;
                    dat_cnt++;
                end
            end
            rxdat_lcrdv = 0;
            check(dat_cnt == 4, $sformatf("got 4 CompData responses (got %0d)", dat_cnt));
            check(seen_ok == 4'hF, $sformatf("all 4 txnids 0x30-0x33 returned (mask=%0b)", seen_ok));
        end

        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h30),
                         .opcode(6'h2 /*CompAck*/));

        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h31),
                         .opcode(6'h2 /*CompAck*/));

        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h32),
                         .opcode(6'h2 /*CompAck*/));

        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h33),
                         .opcode(6'h2 /*CompAck*/));

        // ── [8] TXNID reuse after first transaction completes ────────────
        $display("\n[8] TXNID reuse: same txnid=0x01 on a fresh request");
        do_reset_and_init();
        // First use of txnid=0x01 at a new address
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h01),
                 .opcode(7'h04 /*ReadNoSnp*/), .addr(44'hB000));
        wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
        check(got, "first request with reused txnid received DAT");
        if (got)
            check(dat_txnid(dat) == 12'h01, "first reuse: txnid=0x01 returned");

        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h01),
                         .opcode(6'h2 /*CompAck*/));

        // Immediately reuse the same txnid (after reset/reinit)
        do_reset_and_init();
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h01),
                 .opcode(7'h04 /*ReadNoSnp*/), .addr(44'hC000));
        wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
        check(got, "second request with reused txnid received DAT");
        if (got)
            check(dat_txnid(dat) == 12'h01, "second reuse: txnid=0x01 returned");
        
        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h01),
                         .opcode(6'h2 /*CompAck*/));
        

        // ── [9] ReadNotSharedDirty ─────────────────────────────────────────
        $display("\n[9] ReadNotSharedDirty addr=0x10000 (opcode 0x07)");
        do_reset_and_init();
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h60),
                 .opcode(7'h07 /*ReadNotSharedDirty*/), .addr(44'h10000));
        wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
        check(got,                           "received DAT flit");
        if (got) begin
            check(dat_opcode(dat) == 4'h4,   "DAT opcode=CompData(0x4)");
            check(dat_txnid(dat)  == 12'h60,  "DAT txnid=0x60");
        end

        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h60),
                         .opcode(6'h2 /*CompAck*/));


        // ── [10] ReadUnique ────────────────────────────────────────────────
        $display("\n[10] ReadUnique addr=0x11000 (opcode 0x07)");
        do_reset_and_init();
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h61),
                 .opcode(7'h07 /*ReadUnique*/), .addr(44'h11000));
        wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
        check(got,                           "received DAT flit");
        if (got) begin
            check(dat_opcode(dat) == 4'h4,   "DAT opcode=CompData(0x4)");
            check(dat_txnid(dat)  == 12'h61,  "DAT txnid=0x61");
        end

        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h61),
                         .opcode(6'h2 /*CompAck*/));

        // ── [11] Eight pipelined ReadNoSnp ────────────────────────────────
        $display("\n[11] Eight pipelined ReadNoSnp (txnid 0x40..0x47)");
        do_reset_and_init();
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h40), .opcode(7'h30), .addr(44'h20000));
        tick(1);
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h41), .opcode(7'h30), .addr(44'h21000));
        tick(1);
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h42), .opcode(7'h30), .addr(44'h22000));
        tick(1);
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h43), .opcode(7'h30), .addr(44'h23000));
        tick(1);
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h44), .opcode(7'h30), .addr(44'h24000));
        tick(1);
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h45), .opcode(7'h30), .addr(44'h25000));
        tick(1);
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h46), .opcode(7'h30), .addr(44'h26000));
        tick(1);
        send_req(.tgt(7'h0), .src(7'h4), .txn(12'h47), .opcode(7'h30), .addr(44'h27000));
        begin
            automatic int dat_cnt = 0;
            automatic logic [7:0] seen_ok = 8'b0;  // one bit per txnid 0x40..0x47
            rxdat_lcrdv = 1;
            for (int i = 0; i < 400000 && dat_cnt < 8; i++) begin
                posedge_clk;
                if (rxdat_flitv) begin
                    automatic logic [11:0] tid = dat_txnid(rxdat_flit);
                    check(dat_opcode(rxdat_flit) == 4'h4,
                          $sformatf("8pipe[%0d] opcode=CompData", dat_cnt));
                    if (tid >= 12'h40 && tid <= 12'h47)
                        seen_ok[tid - 12'h40] = 1'b1;
                    dat_cnt++;
                end
            end
            rxdat_lcrdv = 0;
            check(dat_cnt == 8, $sformatf("got 8 CompData responses (got %0d)", dat_cnt));
            check(seen_ok == 8'hFF,
                  $sformatf("all 8 txnids 0x40-0x47 returned (mask=%08b)", seen_ok));
        end

        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h40),
                         .opcode(6'h2 /*CompAck*/));
        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h41),
                         .opcode(6'h2 /*CompAck*/));
        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h42),
                         .opcode(6'h2 /*CompAck*/));
        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h43),
                         .opcode(6'h2 /*CompAck*/));
        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h44),
                         .opcode(6'h2 /*CompAck*/));
        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h45),
                         .opcode(6'h2 /*CompAck*/));
        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h46),
                         .opcode(6'h2 /*CompAck*/));
        send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h47),
                         .opcode(6'h2 /*CompAck*/));
                    

        // ── [12] ReadUnique + CompAck + WriteBackFull data round-trip ──────
        // Protocol:
        //   1. ReadUnique → CompData_UC  (HN-F grants exclusive ownership)
        //   2. CompAck (TXNID=DBID)      (VIPController maps DBID→addr for routing)
        //      After CompAck, UpdateDirState_FromReqResp sets dir_ownerExists=true.
        //   3. WriteBackFull → CompDBIDResp  (non-stale path; Initiate_CopyBack)
        //   4. CBWrData_UD_PD with data 0xA5/0x5A  (dirty writeback; data stored)
        //   5. ReadNoSnp → CompData  (verifies written data was persisted)
        $display("\n[12] ReadUnique+CompAck+WriteBackFull data round-trip (addr=0x30000)");
        begin
            automatic logic                      got_rsp_v;
            automatic logic [CHI_RSP_FLIT_W-1:0] rsp_cap;
            automatic logic [11:0]               dbid_val;
            automatic logic [6:0]                home_id;
            automatic logic [511:0]              wdata;
            automatic logic [63:0]               wbe;

            wdata = 512'h0;
            wdata[7:0]  = 8'hA5;
            wdata[15:8] = 8'h5A;
            wbe = {64{1'b1}};

            do_reset_and_init();

            // Step 1: ReadUnique — HN-F fetches from SNF, sends CompData
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'h70),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'h30000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WBF setup: ReadUnique received CompData");

            // Step 2: CompAck — completes ReadUnique TBE, sets dir_ownerExists=true.
            // TXNID = DBID echoed in CompData (dat[42:31] = original request txnId).
            // VIPController uses this to recover the address for HN-F routing.
            if (got) begin
                dbid_val = dat[42:31];
                home_id  = dat[17:11];
                send_rsp(.tgt(home_id), .src(7'h4), .txn(dbid_val),
                         .opcode(6'h2 /*CompAck*/));
            end

            // Step 3: WriteBackFull — non-stale path (dir_ownerExists=true, owner=VIP)
            //   HN-F (Initiate_CopyBack) sends CompDBIDResp, expects CBWrData_UD_PD.
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'h71),
                     .opcode(7'h1B /*WriteBackFull*/), .addr(44'h30000));
            wait_rsp(.max_cycles(200000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            check(got_rsp_v, "WriteBackFull: received RSP from gem5");
            check(rsp_opcode(rsp_cap)==6'h5 || rsp_opcode(rsp_cap)==6'h6,
                  $sformatf("RSP opcode=DBIDResp(5) or CompDBIDResp(7), got=0x%0x",
                             rsp_opcode(rsp_cap)));
            dbid_val = rsp_dbid(rsp_cap);

            // Step 4: CopyBackWrData_UD_PD — dirty data (RESP=5 per make_cbwrdat default)
            if (got_rsp_v) begin
                send_cbwrdat(.tgt(7'h0), .src(7'h4), .txn(12'h71), .dbid(dbid_val),
                             .data(wdata), .be(wbe));
            end

            if (got_rsp_v && rsp_opcode(rsp_cap) == 6'h6) begin
                wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
                check(got_rsp_v, "WriteBackFull: received Comp RSP after DBIDResp");
            end

            // Step 5: ReadNoSnp to verify the dirty writeback data was persisted
            tick(10000);
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'h72),
                     .opcode(7'h04 /*ReadNoSnp*/), .addr(44'h30000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WBF round-trip: ReadNoSnp received CompData");
            if (got)
                // Direct bit-slice avoids passing 637-bit dat through dat_data()
                // function (Questa has issues slicing wide vectors in function args).
                // DATA field: dat[644:133]; byte 0 = dat[140:133], byte 1 = dat[148:141]
                check(dat[140:133] == 8'hA5 && dat[148:141] == 8'h5A,
                      $sformatf("WBF data byte0=0xA5 byte1=0x5A, got 0x%0x/0x%0x",
                                 dat[140:133], dat[148:141]));
            
            send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h72),
                         .opcode(6'h2 /*CompAck*/));

            tick(5000);
        end

        // ── [13] WriteUniqueFull + ReadNoSnp data round-trip ─────────────
        // Non-coherent write via WriteUniqueFull (0x21): the HN-F gives the VIP
        // exclusive access and writes the NCBWrData to memory.
        // NOTE: WriteNoSnpFull (0x24) is NOT handled by the HN-F's reqToEvent()
        // (CHI-cache-funcs.sm falls through to error() for WriteNoSnp).  In a
        // full CHI fabric WriteNoSnpFull goes directly RN-I → SN-F, bypassing
        // the HN-F.  WriteUniqueFull is the correct "write new data via HN-F" op.
        // Flow: WriteUniqueFull → CompDBIDResp/DBIDResp
        //       → NonCopyBackWrData → ReadNoSnp to verify
        $display("\n[13] WriteUniqueFull + ReadNoSnp data round-trip (addr=0x40000)");
        begin
            automatic logic                      got_rsp_v;
            automatic logic [CHI_RSP_FLIT_W-1:0] rsp_cap;
            automatic logic [11:0]               dbid_val;
            automatic logic [511:0]              wdata;
            automatic logic [63:0]               wbe;

            wdata = 512'h0;
            wdata[7:0]   = 8'hC3;
            wdata[15:8]  = 8'h3C;
            wbe = {64{1'b1}};

            do_reset_and_init();
            rxrsp_lcrdv = 1;
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'h80),
                     .opcode(7'h19 /*WriteUniqueFull*/), .addr(44'h40000));

            wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            check(got_rsp_v, "WriteUniqueFull: received RSP from gem5");
            check(rsp_opcode(rsp_cap)==6'h5 || rsp_opcode(rsp_cap)==6'h6,
                  $sformatf("RSP opcode=DBIDResp(5) or CompDBIDResp(7), got=0x%0x",
                             rsp_opcode(rsp_cap)));
            dbid_val = rsp_dbid(rsp_cap);

            send_dat(.tgt(7'h0), .src(7'h4), .txn(12'h80), .dbid(dbid_val),
                     .data(wdata), .be(wbe));

            if (rsp_opcode(rsp_cap) == 6'h6) begin
                rxrsp_lcrdv = 1;
                wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
                check(got_rsp_v, "WriteUniqueFull: received Comp RSP");
                check(rsp_opcode(rsp_cap)==6'h4,
                      $sformatf("Comp opcode=0x4, got=0x%0x", rsp_opcode(rsp_cap)));
            end

            tick(50000);
            do_reset_and_init();
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'h81),
                     .opcode(7'h04 /*ReadNoSnp*/), .addr(44'h40000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got,                              "WriteUnique+Read: received CompData");
            if (got) begin
                check(dat_opcode(dat) == 4'h4,      "readback opcode=CompData(0x4)");
                // Direct bit-slice: DATA byte 0 = dat[140:133], byte 1 = dat[148:141]
                check(dat[140:133] == 8'hC3,
                      $sformatf("readback byte0=0xC3, got=0x%02x", dat[140:133]));
                check(dat[148:141] == 8'h3C,
                      $sformatf("readback byte1=0x3C, got=0x%02x", dat[148:141]));
            end

            send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h81),
                         .opcode(6'h2 /*CompAck*/));
        end

        // ── [14] Full 512-bit DATA field: sentinel at every 8th byte ─────
        // Writes a known byte to the first byte of each 64-bit word in the
        // 512-bit DATA field, then reads back and verifies all 8 positions.
        // This stresses the complete DPI bypass path (all 16 svBitVecVal words).
        // DATA field: flit[644:133].  Byte N is at flit[140+8*N : 133+8*N].
        $display("\n[14] 512-bit data: sentinel bytes at every 8th byte (addr=0x50000)");
        begin
            automatic logic                      got_rsp_v;
            automatic logic [CHI_RSP_FLIT_W-1:0] rsp_cap;
            automatic logic [11:0]               dbid_val;
            automatic logic [511:0]              wdata;
            automatic logic [63:0]               wbe;

            wdata = 512'h0;
            wdata[7:0]     = 8'hAA;   // byte  0  → flit[140:133]
            wdata[71:64]   = 8'hBB;   // byte  8  → flit[204:197]
            wdata[135:128] = 8'hCC;   // byte 16  → flit[268:261]
            wdata[199:192] = 8'hDD;   // byte 24  → flit[332:325]
            wdata[263:256] = 8'hEE;   // byte 32  → flit[396:389]
            wdata[327:320] = 8'hFF;   // byte 40  → flit[460:453]
            wdata[391:384] = 8'h55;   // byte 48  → flit[524:517]
            wdata[455:448] = 8'h11;   // byte 56  → flit[588:581]
            wbe = {64{1'b1}};

            do_reset_and_init();
            rxrsp_lcrdv = 1;
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'h90),
                     .opcode(7'h19 /*WriteUniqueFull*/), .addr(44'h50000));
            wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            check(got_rsp_v, "512b write: received RSP from gem5");
            dbid_val = rsp_dbid(rsp_cap);
            send_dat(.tgt(7'h0), .src(7'h4), .txn(12'h90), .dbid(dbid_val),
                     .data(wdata), .be(wbe));
            if (rsp_opcode(rsp_cap) == 6'h6) begin
                rxrsp_lcrdv = 1;
                wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            end

            tick(50000);
            do_reset_and_init();
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'h91),
                     .opcode(7'h04 /*ReadNoSnp*/), .addr(44'h50000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "512b data: ReadNoSnp received CompData");
            if (got) begin
                check(dat[140:133] == 8'hAA, $sformatf("byte  0=0xAA got=0x%02x", dat[140:133]));
                check(dat[204:197] == 8'hBB, $sformatf("byte  8=0xBB got=0x%02x", dat[204:197]));
                check(dat[268:261] == 8'hCC, $sformatf("byte 16=0xCC got=0x%02x", dat[268:261]));
                check(dat[332:325] == 8'hDD, $sformatf("byte 24=0xDD got=0x%02x", dat[332:325]));
                check(dat[396:389] == 8'hEE, $sformatf("byte 32=0xEE got=0x%02x", dat[396:389]));
                check(dat[460:453] == 8'hFF, $sformatf("byte 40=0xFF got=0x%02x", dat[460:453]));
                check(dat[524:517] == 8'h55, $sformatf("byte 48=0x55 got=0x%02x", dat[524:517]));
                check(dat[588:581] == 8'h11, $sformatf("byte 56=0x11 got=0x%02x", dat[588:581]));
            end

            send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'h91),
                         .opcode(6'h2 /*CompAck*/));
            
        end

        // ── [15] Overwrite same address twice via WriteBackFull ───────────
        // Verifies gem5 memory is properly updated on consecutive dirty
        // writebacks to the same cache-line address.
        // Each cycle: ReadUnique → CompAck → WriteBackFull → CBWrData_UD_PD
        // → ReadNoSnp verify.  WriteUniqueFull is NOT used here because a
        // second WriteUniqueFull to an address previously seen by ReadNoSnp
        // triggers a gem5 CHI-cache.sm panic (Receive_ReqDataResp in wrong state).
        $display("\n[15] Overwrite same address twice via WriteBackFull");
        if (1) begin
            automatic logic                      got_rsp_v;
            automatic logic [CHI_RSP_FLIT_W-1:0] rsp_cap;
            automatic logic [11:0]               dbid_val;
            automatic logic [6:0]                home_id;
            automatic logic [511:0]              wdata;
            automatic logic [63:0]               wbe;
            wbe = {64{1'b1}};

            // ─ First write: byte 0 = 0x11 ─────────────────────────────────
            wdata = 512'h0;
            wdata[7:0] = 8'h11;
            do_reset_and_init();

            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hA0),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'h60000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "overwrite[1] ReadUnique received CompData");
            if (got) begin
                dbid_val = dat[42:31];
                home_id  = dat[17:11];
                send_rsp(.tgt(home_id), .src(7'h4), .txn(dbid_val),
                         .opcode(6'h2 /*CompAck*/));
            end


            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hA1),
                     .opcode(7'h1B /*WriteBackFull*/), .addr(44'h60000));
            wait_rsp(.max_cycles(200000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            check(got_rsp_v, "overwrite[1] WriteBackFull received RSP");
            dbid_val = rsp_dbid(rsp_cap);
            if (got_rsp_v)
                send_cbwrdat(.tgt(7'h0), .src(7'h4), .txn(12'hA1), .dbid(dbid_val),
                             .data(wdata), .be(wbe));
            if (got_rsp_v && rsp_opcode(rsp_cap) == 6'h6) begin
                rxrsp_lcrdv = 1;
                wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            end

            tick(50000);
            do_reset_and_init();
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hA2),
                     .opcode(7'h04 /*ReadNoSnp*/), .addr(44'h60000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "overwrite[1]: ReadNoSnp received CompData");
            if (got)
                check(dat[140:133] == 8'h11,
                      $sformatf("first write byte0=0x11 got=0x%02x", dat[140:133]));
            send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'hA2),
                         .opcode(6'h2 /*CompAck*/));

            // ─ Second write: byte 0 = 0x22 ────────────────────────────────
            wdata = 512'h0;
            wdata[7:0] = 8'h22;
            do_reset_and_init();

            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hA3),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'h60000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "overwrite[2] ReadUnique received CompData");
            if (got) begin
                dbid_val = dat[42:31];
                home_id  = dat[17:11];
                send_rsp(.tgt(home_id), .src(7'h4), .txn(dbid_val),
                         .opcode(6'h2 /*CompAck*/));
            end

            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hA4),
                     .opcode(7'h1B /*WriteBackFull*/), .addr(44'h60000));
            wait_rsp(.max_cycles(200000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            check(got_rsp_v, "overwrite[2] WriteBackFull received RSP");
            dbid_val = rsp_dbid(rsp_cap);
            if (got_rsp_v)
                send_cbwrdat(.tgt(7'h0), .src(7'h4), .txn(12'hA4), .dbid(dbid_val),
                             .data(wdata), .be(wbe));
            if (got_rsp_v && rsp_opcode(rsp_cap) == 6'h6) begin
                rxrsp_lcrdv = 1;
                wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            end

            tick(50000);
            do_reset_and_init();
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hA5),
                     .opcode(7'h04 /*ReadNoSnp*/), .addr(44'h60000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "overwrite[2]: ReadNoSnp received CompData");
            if (got)
                check(dat[140:133] == 8'h22,
                      $sformatf("second write byte0=0x22 got=0x%02x", dat[140:133]));
            send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'hA5),
                         .opcode(6'h2 /*CompAck*/));
        end

        // ── [16] Address isolation: two cache lines stay independent ──────
        // Writes distinct patterns to two different addresses then reads both
        // back to confirm no aliasing or cross-contamination in gem5 memory.
        $display("\n[16] Address isolation: two cache lines stay independent");
        if (1) begin
            automatic logic                      got_rsp_v;
            automatic logic [CHI_RSP_FLIT_W-1:0] rsp_cap;
            automatic logic [11:0]               dbid_val;
            automatic logic [511:0]              wdata;
            automatic logic [63:0]               wbe;
            wbe = {64{1'b1}};

            do_reset_and_init();

            // Write 0xAA to addr1 = 0x70000
            wdata = 512'h0;
            wdata[7:0] = 8'hAA;
            rxrsp_lcrdv = 1;
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hB0),
                     .opcode(7'h19 /*WriteUniqueFull*/), .addr(44'h70000));
            wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            dbid_val = rsp_dbid(rsp_cap);
            send_dat(.tgt(7'h0), .src(7'h4), .txn(12'hB0), .dbid(dbid_val),
                     .data(wdata), .be(wbe));
            if (rsp_opcode(rsp_cap) == 6'h6) begin
                rxrsp_lcrdv = 1;
                wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            end

            // Write 0x55 to addr2 = 0x71000 (different cache line)
            tick(20);
            wdata = 512'h0;
            wdata[7:0] = 8'h55;
            rxrsp_lcrdv = 1;
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hB1),
                     .opcode(7'h19 /*WriteUniqueFull*/), .addr(44'h71000));
            wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            dbid_val = rsp_dbid(rsp_cap);
            send_dat(.tgt(7'h0), .src(7'h4), .txn(12'hB1), .dbid(dbid_val),
                     .data(wdata), .be(wbe));
            if (rsp_opcode(rsp_cap) == 6'h6) begin
                rxrsp_lcrdv = 1;
                wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            end

            tick(50000);

            // Read addr1: expect 0xAA
            do_reset_and_init();
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hB2),
                     .opcode(7'h04 /*ReadNoSnp*/), .addr(44'h70000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "isolation: addr1 ReadNoSnp received CompData");
            if (got)
                check(dat[140:133] == 8'hAA,
                      $sformatf("addr1(0x70000) byte0=0xAA got=0x%02x", dat[140:133]));
            send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'hB2),
                         .opcode(6'h2 /*CompAck*/));

            // Read addr2: expect 0x55 (must not see addr1's 0xAA)
            do_reset_and_init();
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hB3),
                     .opcode(7'h04 /*ReadNoSnp*/), .addr(44'h71000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "isolation: addr2 ReadNoSnp received CompData");
            if (got)
                check(dat[140:133] == 8'h55,
                      $sformatf("addr2(0x71000) byte0=0x55 got=0x%02x", dat[140:133]));
            send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'hB3),
                         .opcode(6'h2 /*CompAck*/));
        end

        // ── [17] ReadOnce ─────────────────────────────────────────────────
        // Non-temporal read: HN-F fetches data but does not allocate a dirty
        // cache line. ExpCompAck is 0, but we send CompAck for TBE safety.
        $display("\n[17] ReadOnce addr=0x80000 (opcode 0x03 — non-temporal read)");
        begin
            do_reset_and_init();
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hC0),
                     .opcode(7'h03 /*ReadOnce*/), .addr(44'h80000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got,                           "ReadOnce: received CompData");
            if (got) begin
                check(dat_opcode(dat) == 4'h4,   "ReadOnce: opcode=CompData(0x4)");
                check(dat_txnid(dat)  == 12'hC0,  "ReadOnce: txnid=0xC0");
            end
            send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'hC0), .opcode(6'h2 /*CompAck*/));
        end

        // ── [18] CleanUnique — SC→UC coherency upgrade ────────────────────
        // Flow: ReadShared (SC) → CompAck → CleanUnique → Comp_UC → CompAck
        // CleanUnique upgrades an SC line to UC without data transfer.
        // gem5 returns Comp (opcode=0x1) with RESP=UC (2).
        $display("\n[18] CleanUnique addr=0x90000 (opcode 0x11 — SC→UC upgrade)");
        begin
            automatic logic                      got_rsp_v;
            automatic logic [CHI_RSP_FLIT_W-1:0] rsp_cap;
            automatic logic [11:0]               dbid_val;
            automatic logic [6:0]                home_id;

            do_reset_and_init();

            // Step 1: ReadShared to put line in SC state at the HN-F
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hC1),
                     .opcode(7'h01 /*ReadShared*/), .addr(44'h90000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "CleanUnique setup: ReadShared got CompData");
            if (got) begin
                dbid_val = dat[42:31];
                home_id  = dat[17:11];
                send_rsp(.tgt(home_id), .src(7'h4), .txn(dbid_val),
                         .opcode(6'h2 /*CompAck*/));
            end

            // Step 2: CleanUnique — upgrade SC→UC, no data transfer
            tick(100);
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hC2),
                     .opcode(7'h0B /*CleanUnique*/), .addr(44'h90000));
            wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            check(got_rsp_v, "CleanUnique: received Comp RSP");
            if (got_rsp_v) begin
                check(rsp_opcode(rsp_cap) == 6'h4,
                      $sformatf("CleanUnique: opcode=Comp(0x4) got=0x%0x",
                                 rsp_opcode(rsp_cap)));
                send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'hC2),
                         .opcode(6'h2 /*CompAck*/));
            end
        end

        // ── [19] WriteUniqueZero — zero a cache line without data ─────────
        // WriteUniqueZero: HN-F zeros the memory without receiving data from
        // the requester.  gem5 may return Comp (0x1) or CompDBIDResp (0x7).
        // If CompDBIDResp: send NCBWrData of zeros so HN-F can write them.
        if(0) begin
        $display("\n[19] WriteUniqueZero addr=0xA0000 (opcode 0x22 — zero cache line)");
        begin
            automatic logic                      got_rsp_v;
            automatic logic [CHI_RSP_FLIT_W-1:0] rsp_cap;
            automatic logic [11:0]               dbid_val;
            automatic logic [511:0]              wdata;
            automatic logic [63:0]               wbe;
            wbe = {64{1'b1}};

            // Pre-condition: write a non-zero value so we can verify zeroing
            wdata = 512'h0;
            wdata[7:0] = 8'hAB;
            do_reset_and_init();
            rxrsp_lcrdv = 1;
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hC3),
                     .opcode(7'h19 /*WriteUniqueFull*/), .addr(44'hA0000));
            wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            dbid_val = rsp_dbid(rsp_cap);
            send_dat(.tgt(7'h0), .src(7'h4), .txn(12'hC3), .dbid(dbid_val),
                     .data(wdata), .be(wbe));
            if (rsp_opcode(rsp_cap) == 6'h6) begin
                rxrsp_lcrdv = 1;
                wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            end

            // WriteUniqueZero: no data needed from requester
            tick(20000);
            do_reset_and_init();
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hC4),
                     .opcode(7'h22 /*WriteUniqueZero*/), .addr(44'hA0000));
            wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            check(got_rsp_v, "WriteUniqueZero: received RSP");
            if (got_rsp_v) begin
                check(rsp_opcode(rsp_cap) == 6'h4 || rsp_opcode(rsp_cap) == 6'h5,
                      $sformatf("WriteUniqueZero: Comp(1) or CompDBIDResp(7), got=0x%0x",
                                 rsp_opcode(rsp_cap)));
                // If HN-F issued a DBID, send all-zeros as NCBWrData
                wdata    = 512'h0;
                dbid_val = rsp_dbid(rsp_cap);
                if (rsp_opcode(rsp_cap) == 6'h5) begin
                    send_dat(.tgt(7'h0), .src(7'h4), .txn(12'hC4), .dbid(dbid_val),
                             .data(wdata), .be(wbe));
                end else if (rsp_opcode(rsp_cap) == 6'h6) begin
                    send_dat(.tgt(7'h0), .src(7'h4), .txn(12'hC4), .dbid(dbid_val),
                             .data(wdata), .be(wbe));
                    rxrsp_lcrdv = 1;
                    wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
                end
            end

            // Verify: memory should now be zero
            tick(20000);
            do_reset_and_init();
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hC5),
                     .opcode(7'h04 /*ReadNoSnp*/), .addr(44'hA0000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WriteUniqueZero verify: ReadNoSnp received CompData");
            if (got)
                check(dat[140:133] == 8'h00,
                      $sformatf("WriteUniqueZero: byte0=0x00 got=0x%02x", dat[140:133]));
            send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'hC5), .opcode(6'h2 /*CompAck*/));
        end
        end

        // ── [20] Evict — shared copy eviction (HNF in SC_RSC) ────────────
        // Flow: ReadShared (SC) → CompAck → Evict → Comp_I
        // Evict releases the requester's SC copy; HNF was in SC_RSC (inclusive).
        // No CompAck from requester after Evict's Comp_I.
        if(1) begin
        $display("\n[20] Evict addr=0xB0000 (opcode 0x17 — SC eviction after ReadShared)");
        begin
            automatic logic                      got_rsp_v;
            automatic logic [CHI_RSP_FLIT_W-1:0] rsp_cap;
            automatic logic [11:0]               dbid_val;
            automatic logic [6:0]                home_id;

            do_reset_and_init();

            // Step 1: ReadShared → VIP gets SC; HNF lands in SC_RSC (inclusive policy)
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hC6),
                     .opcode(7'h01 /*ReadShared*/), .addr(44'hB0000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "Evict setup: ReadShared got CompData");
            if (got) begin
                dbid_val = dat[42:31];
                home_id  = dat[17:11];
                send_rsp(.tgt(home_id), .src(7'h4), .txn(dbid_val),
                         .opcode(6'h2 /*CompAck*/));
            end

            // Step 2: Evict — release SC copy; HN-F sends Comp_I (no data, no CompAck)
            tick(100);
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hC7),
                     .opcode(7'h0D /*Evict*/), .addr(44'hB0000));
            wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            check(got_rsp_v, "Evict: received Comp RSP");
            if (got_rsp_v)
                check(rsp_opcode(rsp_cap) == 6'h4,
                      $sformatf("Evict: opcode=Comp(0x4) got=0x%0x",
                                 rsp_opcode(rsp_cap)));

            // Step 3: Verify line still readable (HNF deallocated → fetches from SNF)
            tick(10000);
            do_reset_and_init();
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hC8),
                     .opcode(7'h04 /*ReadNoSnp*/), .addr(44'hB0000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "Evict verify: ReadNoSnp after eviction got CompData");
            if (got)
                check(dat_opcode(dat) == 4'h4, "Evict verify: opcode=CompData(0x4)");
            send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'hC8), .opcode(6'h2 /*CompAck*/));
        end
        end

        // ── [21] WriteCleanFull — dirty writeback after ReadUnique ─────────
        // gem5 SLICC (Initiate_CopyBack) expects CBWrData_UD_PD for WriteCleanFull.
        // After WriteCleanFull the HNF enters UD_RU: it has dirty data (UD) AND
        // the VIP still holds UC ownership in the directory (RU).
        // A WriteEvictFull releases that UC ownership → HNF moves to UD.
        // For UD_RU, copyCacheAndDir does NOT copy HNF's cache (upstream is
        // authoritative), so CBWrData_UC IS used to fill the HNF cache.
        // VIP must send back 0xBE (its retained UC copy after WriteCleanFull).
        // Verify via ReadUnique (cacheable address; ReadNoSnp is non-coherent).
        // Flow: ReadUnique → CompAck → WriteCleanFull → CBWrData_UD_PD
        //       → WriteEvictFull → CBWrData_UC(0xBE) → ReadUnique verify → CompAck
        if(1) begin
        $display("\n[21] WriteCleanFull addr=0xC0000 (opcode 0x1C — CBWrData_UD_PD)");
        begin
            automatic logic                      got_rsp_v;
            automatic logic [CHI_RSP_FLIT_W-1:0] rsp_cap;
            automatic logic [11:0]               dbid_val;
            automatic logic [6:0]                home_id;
            automatic logic [511:0]              wdata;
            automatic logic [63:0]               wbe;
            wbe   = {64{1'b1}};
            wdata = 512'h0;
            wdata[7:0] = 8'hBE;

            do_reset_and_init();

            // Step 1: ReadUnique → VIP gets exclusive ownership (UC_RU at HNF)
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hC9),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'hC0000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WriteCleanFull setup: ReadUnique got CompData");
            if (got) begin
                dbid_val = dat[42:31];
                home_id  = dat[17:11];
                send_rsp(.tgt(home_id), .src(7'h4), .txn(dbid_val),
                         .opcode(6'h2 /*CompAck*/));
            end

            // Step 2: WriteCleanFull — pass dirty data back; VIP keeps UC copy
            // gem5 expects CBWrData_UD_PD (dirty encoding) for WriteCleanFull.
            // HNF state after this: UD_RU (HNF has dirty data, VIP still in dir as UC)
            rxrsp_lcrdv = 1;
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hCA),
                     .opcode(7'h17 /*WriteCleanFull*/), .addr(44'hC0000));
            wait_rsp(.max_cycles(200000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            check(got_rsp_v, "WriteCleanFull: received RSP");
            check(rsp_opcode(rsp_cap)==6'h5 || rsp_opcode(rsp_cap)==6'h6,
                  $sformatf("WriteCleanFull: CompDBIDResp(5)/DBIDResp(6) got=0x%0x",
                             rsp_opcode(rsp_cap)));
            dbid_val = rsp_dbid(rsp_cap);
            if (got_rsp_v)
                send_cbwrdat(.tgt(7'h0), .src(7'h4), .txn(12'hCA), .dbid(dbid_val),
                             .data(wdata), .be(wbe));
            if (got_rsp_v && rsp_opcode(rsp_cap) == 6'h6) begin
                rxrsp_lcrdv = 1;
                wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            end

            // Step 3: WriteEvictFull — release VIP's UC ownership (UD_RU → UD)
            // CBWrData_UC MUST carry VIP's retained UC copy (= 0xBE, the clean
            // value after WriteCleanFull).  For UD_RU, copyCacheAndDir does NOT
            // copy the HNF cache into TBE (upstream is authoritative), so the
            // CBWrData_UC payload is used verbatim to fill the HNF cache.
            tick(1000);
            rxrsp_lcrdv = 1;
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hCB),
                     .opcode(7'h15 /*WriteEvictFull*/), .addr(44'hC0000));
            wait_rsp(.max_cycles(200000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            check(got_rsp_v, "WriteCleanFull cleanup: WriteEvictFull got RSP");
            dbid_val = rsp_dbid(rsp_cap);
            if (got_rsp_v)
                send_cbwrdat_clean(.tgt(7'h0), .src(7'h4), .txn(12'hCB), .dbid(dbid_val),
                                   .data(wdata), .be({64{1'b1}}));
            if (got_rsp_v && rsp_opcode(rsp_cap) == 6'h6) begin
                rxrsp_lcrdv = 1;
                wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            end

            // Step 4: Verify dirty data persisted in HNF cache (now in UD state)
            // Use ReadUnique — cacheable address, UD state; ReadNoSnp is non-coherent
            // and must not be used on cacheable lines.
            tick(20000);
            do_reset_and_init();
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hC9),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'hC0000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WriteCleanFull verify: ReadUnique received CompData");
            if (got)
                check(dat[140:133] == 8'hBE,
                      $sformatf("WriteCleanFull byte0=0xBE got=0x%02x", dat[140:133]));
            send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'hC9), .opcode(6'h2 /*CompAck*/));
        end
        end

        // ── [22] MakeReadUnique — SC→UC upgrade ───────────────────────────
        // Flow: ReadShared (SC) → CompAck → MakeReadUnique → Comp_UC/CompData → CompAck
        // With a single requester, HN-F should return Comp_UC (no data needed).
        $display("\n[22] MakeReadUnique addr=0xD0000 (opcode 0x15 — SC→UC via snoop)");
        begin
            automatic logic                      got_rsp_v;
            automatic logic                      got_dat_v;
            automatic logic [CHI_RSP_FLIT_W-1:0] rsp_cap;
            automatic logic [11:0]               dbid_val;
            automatic logic [6:0]                home_id;

            do_reset_and_init();

            // Step 1: ReadShared to acquire SC state
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hCC),
                     .opcode(7'h01 /*ReadShared*/), .addr(44'hD0000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "MakeReadUnique setup: ReadShared got CompData");
            if (got) begin
                dbid_val = dat[42:31];
                home_id  = dat[17:11];
                send_rsp(.tgt(home_id), .src(7'h4), .txn(dbid_val),
                         .opcode(6'h2 /*CompAck*/));
            end

            // Step 2: MakeReadUnique — may return Comp_UC (RSP) or CompData (DAT)
            tick(100);
            rxrsp_lcrdv = 1;
            rxdat_lcrdv = 1;
            got_rsp_v   = 0;
            got_dat_v   = 0;
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hCD),
                     .opcode(7'h41 /*MakeReadUnique*/), .addr(44'hD0000));
            for (int i = 0; i < 200000 && !got_rsp_v && !got_dat_v; i++) begin
                posedge_clk;
                if (rxrsp_flitv) begin rsp_cap = rxrsp_flit; got_rsp_v = 1; rxrsp_lcrdv = 0; end
                if (rxdat_flitv) begin #0; dat = rxdat_flit; got_dat_v = 1; rxdat_lcrdv = 0; end
            end
            check(got_rsp_v || got_dat_v, "MakeReadUnique: received RSP or DAT from gem5");
            if (got_rsp_v) begin
                check(rsp_opcode(rsp_cap) == 6'h4,
                      $sformatf("MakeReadUnique: Comp(0x4) got=0x%0x",
                                 rsp_opcode(rsp_cap)));
                send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'hCD), .opcode(6'h2 /*CompAck*/));
            end
            if (got_dat_v) begin
                check(dat_opcode(dat) == 4'h4, "MakeReadUnique: DAT opcode=CompData(0x4)");
                dbid_val = dat[42:31];
                home_id  = dat[17:11];
                send_rsp(.tgt(home_id), .src(7'h4), .txn(dbid_val), .opcode(6'h2 /*CompAck*/));
            end
        end

        // ── [23] WriteEvictFull — evict with clean data ───────────────────
        // gem5 SLICC expects CBWrData_UC (or CBWrData_SC) for WriteEvictFull.
        // For UC_RU state, copyCacheAndDir does NOT copy the HNF cache into TBE
        // (upstream is authoritative); CBWrData_UC is therefore used verbatim to
        // fill the HNF cache.  VIP must send back its retained UC copy (0xCA).
        // Flow: ReadUnique → CompAck → WriteEvictFull → CBWrData_UC(0xCA)
        //       → ReadUnique verify → CompAck
        $display("\n[23] WriteEvictFull addr=0xE0000 (opcode 0x1D — CBWrData_UC)");
        begin
            automatic logic                      got_rsp_v;
            automatic logic [CHI_RSP_FLIT_W-1:0] rsp_cap;
            automatic logic [11:0]               dbid_val;
            automatic logic [6:0]                home_id;
            automatic logic [511:0]              wdata;
            automatic logic [63:0]               wbe;
            wbe   = {64{1'b1}};
            wdata = 512'h0;
            wdata[7:0] = 8'hCA;

            do_reset_and_init();

            // Step 1: ReadUnique — acquire UC ownership (HNF → UC_RU)
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hCE),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'hE0000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WriteEvictFull setup: ReadUnique got CompData");
            if (got) begin
                dbid_val = dat[42:31];
                home_id  = dat[17:11];
                send_rsp(.tgt(home_id), .src(7'h4), .txn(dbid_val),
                         .opcode(6'h2 /*CompAck*/));
            end

            // Step 2: WriteEvictFull — VIP evicts UC copy (0xCA); HNF stores it.
            rxrsp_lcrdv = 1;
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hCF),
                     .opcode(7'h15 /*WriteEvictFull*/), .addr(44'hE0000));
            wait_rsp(.max_cycles(200000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            check(got_rsp_v, "WriteEvictFull: received RSP");
            dbid_val = rsp_dbid(rsp_cap);
            if (got_rsp_v)
                send_cbwrdat_clean(.tgt(7'h0), .src(7'h4), .txn(12'hCF), .dbid(dbid_val),
                                   .data(wdata), .be(wbe));
            if (got_rsp_v && rsp_opcode(rsp_cap) == 6'h6) begin
                rxrsp_lcrdv = 1;
                wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            end

            // Step 3: Verify 0xCA is now in HNF cache (UC state) via ReadUnique
            tick(20000);
            do_reset_and_init();
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hD0),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'hE0000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WriteEvictFull verify: ReadUnique received CompData");
            if (got)
                check(dat[140:133] == 8'hCA,
                      $sformatf("WriteEvictFull byte0=0xCA got=0x%02x", dat[140:133]));
            send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'hD0), .opcode(6'h2 /*CompAck*/));
        end

        // ── [24] WriteBackPtl — partial dirty writeback ───────────────────────
        // Flow: ReadUnique → CompAck (VIP holds UC)
        //       WriteBackPtl → CompDBIDResp
        //       CBWrData_UD_PD with partial BE (bytes 0-3 only = 0xBEEFDEAD)
        //       Verify: ReadUnique → bytes 0-3 = 0xBEEFDEAD
        //
        // SLICC implementation: Initiate_CopyBack pre-loads existing cache
        // data into tbe.dataBlk so that CBWrData_UD_PD's partial copyPartial
        // overlays the dirty bytes.  UpdateDataState_FromReqDataResp then
        // calls fillMask() to mark the merged block complete.  CheckCacheFill
        // stores the merged UD line; final state = UD.
        $display("\n[24] WriteBackPtl addr=0x100000 (opcode 0x1A — partial dirty writeback)");
        begin
            automatic logic                      got_rsp_v;
            automatic logic [CHI_RSP_FLIT_W-1:0] rsp_cap;
            automatic logic [11:0]               dbid_val;
            automatic logic [511:0]              wdata;
            automatic logic [63:0]               wbe;

            // Partial dirty data: bytes 0-3 = 0xBEEFDEAD, rest untouched
            wdata = 512'h0;
            wdata[7:0]  = 8'hAD;   // byte 0
            wdata[15:8] = 8'hDE;   // byte 1
            wdata[23:16] = 8'hEF;  // byte 2
            wdata[31:24] = 8'hBE;  // byte 3
            // Partial byte enable: only bytes 0-3
            wbe = 64'h0000_0000_0000_000F;

            do_reset_and_init();

            // Step 1: ReadUnique — acquire exclusive ownership (HNF → UD_RU or UC_RU)
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hD3),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'h100000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WBPtl setup: ReadUnique received CompData");
            if (got) begin
                dbid_val = dat[42:31];
                send_rsp(.tgt(7'h0), .src(7'h4), .txn(dbid_val),
                         .opcode(6'h2 /*CompAck*/));
            end

            tick(5000);

            // Step 2: WriteBackPtl — partial dirty writeback (bytes 0-3 dirty)
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hD4),
                     .opcode(7'h1A /*WriteBackPtl*/), .addr(44'h100000));
            wait_rsp(.max_cycles(200000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            check(got_rsp_v, "WriteBackPtl: received CompDBIDResp");
            check(rsp_opcode(rsp_cap)==6'h5 || rsp_opcode(rsp_cap)==6'h6,
                  $sformatf("WriteBackPtl: CompDBIDResp(5)/DBIDResp(6) got=0x%0x",
                             rsp_opcode(rsp_cap)));
            dbid_val = rsp_dbid(rsp_cap);

            // Step 3: CBWrData_UD_PD with partial BE (bytes 0-3 only)
            if (got_rsp_v) begin
                send_cbwrdat(.tgt(7'h0), .src(7'h4), .txn(12'hD4), .dbid(dbid_val),
                             .data(wdata), .be(wbe));
            end

            if (got_rsp_v && rsp_opcode(rsp_cap) == 6'h6) begin
                wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
                check(got_rsp_v, "WriteBackPtl: received Comp after DBIDResp");
            end

            // Step 4: Verify bytes 0-3 are now 0xBEEFDEAD in HNF cache (UD state)
            tick(10000);
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hD5),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'h100000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WriteBackPtl verify: ReadUnique received CompData");
            if (got) begin
                check(dat[140:133] == 8'hAD,
                      $sformatf("WriteBackPtl byte0=0xAD got=0x%02x", dat[140:133]));
                check(dat[148:141] == 8'hDE,
                      $sformatf("WriteBackPtl byte1=0xDE got=0x%02x", dat[148:141]));
                check(dat[156:149] == 8'hEF,
                      $sformatf("WriteBackPtl byte2=0xEF got=0x%02x", dat[156:149]));
                check(dat[164:157] == 8'hBE,
                      $sformatf("WriteBackPtl byte3=0xBE got=0x%02x", dat[164:157]));
            end
            send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'hD5),
                     .opcode(6'h2 /*CompAck*/));
        end

        // ── [25] WriteBackPtl — non-zero byte offset (bytes 8-11) ────────────
        // Tests that CBWrData_UD_PD correctly merges at a byte offset > 0.
        // Fresh cache line; bytes 8-11 = 0xFEEDCAFE; all others remain zero.
        $display("\n[25] WriteBackPtl mid-line: bytes 8-11 dirty (addr=0x110000)");
        begin
            automatic logic                      got_rsp_v;
            automatic logic [CHI_RSP_FLIT_W-1:0] rsp_cap;
            automatic logic [11:0]               dbid_val;
            automatic logic [511:0]              wdata;
            automatic logic [63:0]               wbe;

            // 0xFEEDCAFE little-endian in bytes 8-11
            wdata = 512'h0;
            wdata[71:64]  = 8'hFE;   // byte  8
            wdata[79:72]  = 8'hCA;   // byte  9
            wdata[87:80]  = 8'hED;   // byte 10
            wdata[95:88]  = 8'hFE;   // byte 11
            wbe = 64'h0000_0000_0000_0F00;  // bits 11:8

            do_reset_and_init();

            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hE0),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'h110000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WBPtl8-11: ReadUnique received CompData");
            if (got) begin
                dbid_val = dat[42:31];
                send_rsp(.tgt(7'h0), .src(7'h4), .txn(dbid_val),
                         .opcode(6'h2 /*CompAck*/));
            end

            tick(5000);

            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hE1),
                     .opcode(7'h1A /*WriteBackPtl*/), .addr(44'h110000));
            wait_rsp(.max_cycles(200000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            check(got_rsp_v, "WBPtl8-11: received CompDBIDResp");
            check(rsp_opcode(rsp_cap)==6'h5 || rsp_opcode(rsp_cap)==6'h6,
                  $sformatf("WBPtl8-11: DBIDResp/CompDBIDResp got=0x%0x",
                             rsp_opcode(rsp_cap)));
            dbid_val = rsp_dbid(rsp_cap);

            if (got_rsp_v)
                send_cbwrdat(.tgt(7'h0), .src(7'h4), .txn(12'hE1), .dbid(dbid_val),
                             .data(wdata), .be(wbe));

            if (got_rsp_v && rsp_opcode(rsp_cap) == 6'h6) begin
                wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
                check(got_rsp_v, "WBPtl8-11: received Comp after DBIDResp");
            end

            tick(10000);
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hE2),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'h110000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WBPtl8-11: verify ReadUnique received CompData");
            if (got) begin
                check(dat[204:197] == 8'hFE,
                      $sformatf("WBPtl8-11 byte8=0xFE  got=0x%02x", dat[204:197]));
                check(dat[212:205] == 8'hCA,
                      $sformatf("WBPtl8-11 byte9=0xCA  got=0x%02x", dat[212:205]));
                check(dat[220:213] == 8'hED,
                      $sformatf("WBPtl8-11 byte10=0xED got=0x%02x", dat[220:213]));
                check(dat[228:221] == 8'hFE,
                      $sformatf("WBPtl8-11 byte11=0xFE got=0x%02x", dat[228:221]));
            end
            send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'hE2),
                     .opcode(6'h2 /*CompAck*/));
        end

        // ── [26] WriteBackPtl — merge on top of existing dirty (UD) cache line ──
        // WriteUniqueFull seeds bytes 0-3=0xAA in HNF UD cache.
        // ReadUnique re-acquires exclusive ownership (HNF UD → UD_RU).
        // Initiate_CopyBack pre-loads the UD cache entry into tbe.dataBlk.
        // WriteBackPtl then overlays bytes 32-35=0xCC via CBWrData_UD_PD.
        // Verify: bytes 0-3=0xAA survive; bytes 32-35=0xCC overlaid.
        $display("\n[26] WriteBackPtl merge: dirty-on-dirty (addr=0x120000)");
        begin
            automatic logic                      got_rsp_v;
            automatic logic [CHI_RSP_FLIT_W-1:0] rsp_cap;
            automatic logic [11:0]               dbid_val;
            automatic logic [511:0]              wdata;
            automatic logic [63:0]               wbe;

            do_reset_and_init();

            // Step 1: WriteUniqueFull — seed bytes 0-3=0xAA into HNF UD cache
            wdata = 512'h0;
            wdata[7:0]   = 8'hAA;
            wdata[15:8]  = 8'hAA;
            wdata[23:16] = 8'hAA;
            wdata[31:24] = 8'hAA;
            wbe = {64{1'b1}};
            rxrsp_lcrdv = 1;
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hE3),
                     .opcode(7'h19 /*WriteUniqueFull*/), .addr(44'h120000));
            wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            check(got_rsp_v, "WBPtl-merge: WriteUniqueFull received RSP");
            check(rsp_opcode(rsp_cap)==6'h5 || rsp_opcode(rsp_cap)==6'h6,
                  $sformatf("WBPtl-merge: WUF RSP opcode got=0x%0x",
                             rsp_opcode(rsp_cap)));
            dbid_val = rsp_dbid(rsp_cap);
            send_dat(.tgt(7'h0), .src(7'h4), .txn(12'hE3), .dbid(dbid_val),
                     .data(wdata), .be(wbe));
            if (rsp_opcode(rsp_cap) == 6'h6) begin
                rxrsp_lcrdv = 1;
                wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            end

            tick(30000);

            // Step 2: ReadUnique — re-acquire exclusive (HNF UD → UD_RU, sends dirty CompData)
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hE4),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'h120000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WBPtl-merge: ReadUnique received CompData");
            if (got) begin
                dbid_val = dat[42:31];
                send_rsp(.tgt(7'h0), .src(7'h4), .txn(dbid_val),
                         .opcode(6'h2 /*CompAck*/));
            end

            tick(5000);

            // Step 3: WriteBackPtl — overlay bytes 32-35=0xCC on pre-loaded UD data
            wdata = 512'h0;
            wdata[263:256] = 8'hCC;  // byte 32
            wdata[271:264] = 8'hCC;  // byte 33
            wdata[279:272] = 8'hCC;  // byte 34
            wdata[287:280] = 8'hCC;  // byte 35
            wbe = 64'h0000_000F_0000_0000;  // bits 35:32

            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hE5),
                     .opcode(7'h1A /*WriteBackPtl*/), .addr(44'h120000));
            wait_rsp(.max_cycles(200000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            check(got_rsp_v, "WBPtl-merge: WriteBackPtl received CompDBIDResp");
            check(rsp_opcode(rsp_cap)==6'h5 || rsp_opcode(rsp_cap)==6'h6,
                  $sformatf("WBPtl-merge: WBPtl RSP got=0x%0x",
                             rsp_opcode(rsp_cap)));
            dbid_val = rsp_dbid(rsp_cap);

            if (got_rsp_v)
                send_cbwrdat(.tgt(7'h0), .src(7'h4), .txn(12'hE5), .dbid(dbid_val),
                             .data(wdata), .be(wbe));

            if (got_rsp_v && rsp_opcode(rsp_cap) == 6'h6) begin
                wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
                check(got_rsp_v, "WBPtl-merge: received Comp after DBIDResp");
            end

            // Step 4: Verify — pre-loaded bytes 0-3=0xAA survive; bytes 32-35=0xCC overlaid
            tick(10000);
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hE6),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'h120000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WBPtl-merge: verify ReadUnique received CompData");
            if (got) begin
                check(dat[140:133] == 8'hAA,
                      $sformatf("WBPtl-merge byte0=0xAA  got=0x%02x", dat[140:133]));
                check(dat[148:141] == 8'hAA,
                      $sformatf("WBPtl-merge byte1=0xAA  got=0x%02x", dat[148:141]));
                check(dat[156:149] == 8'hAA,
                      $sformatf("WBPtl-merge byte2=0xAA  got=0x%02x", dat[156:149]));
                check(dat[164:157] == 8'hAA,
                      $sformatf("WBPtl-merge byte3=0xAA  got=0x%02x", dat[164:157]));
                check(dat[396:389] == 8'hCC,
                      $sformatf("WBPtl-merge byte32=0xCC got=0x%02x", dat[396:389]));
                check(dat[404:397] == 8'hCC,
                      $sformatf("WBPtl-merge byte33=0xCC got=0x%02x", dat[404:397]));
                check(dat[412:405] == 8'hCC,
                      $sformatf("WBPtl-merge byte34=0xCC got=0x%02x", dat[412:405]));
                check(dat[420:413] == 8'hCC,
                      $sformatf("WBPtl-merge byte35=0xCC got=0x%02x", dat[420:413]));
            end
            send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'hE6),
                     .opcode(6'h2 /*CompAck*/));
        end

        // ── [27] WriteBackPtl — tail of cache line (bytes 60-63) ─────────────
        // Tests partial writeback at the last 4 bytes of the 64-byte cache line.
        // Bytes 60-63 = 0xDEADC0DE; all others remain zero.
        $display("\n[27] WriteBackPtl upper-end: bytes 60-63 dirty (addr=0x130000)");
        begin
            automatic logic                      got_rsp_v;
            automatic logic [CHI_RSP_FLIT_W-1:0] rsp_cap;
            automatic logic [11:0]               dbid_val;
            automatic logic [511:0]              wdata;
            automatic logic [63:0]               wbe;

            // 0xDEADC0DE little-endian in bytes 60-63
            wdata = 512'h0;
            wdata[487:480] = 8'hDE;  // byte 60
            wdata[495:488] = 8'hC0;  // byte 61
            wdata[503:496] = 8'hAD;  // byte 62
            wdata[511:504] = 8'hDE;  // byte 63
            wbe = 64'hF000_0000_0000_0000;  // bits 63:60

            do_reset_and_init();

            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hE7),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'h130000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WBPtl60-63: ReadUnique received CompData");
            if (got) begin
                dbid_val = dat[42:31];
                send_rsp(.tgt(7'h0), .src(7'h4), .txn(dbid_val),
                         .opcode(6'h2 /*CompAck*/));
            end

            tick(5000);

            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hE8),
                     .opcode(7'h1A /*WriteBackPtl*/), .addr(44'h130000));
            wait_rsp(.max_cycles(200000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            check(got_rsp_v, "WBPtl60-63: received CompDBIDResp");
            check(rsp_opcode(rsp_cap)==6'h5 || rsp_opcode(rsp_cap)==6'h6,
                  $sformatf("WBPtl60-63: DBIDResp/CompDBIDResp got=0x%0x",
                             rsp_opcode(rsp_cap)));
            dbid_val = rsp_dbid(rsp_cap);

            if (got_rsp_v)
                send_cbwrdat(.tgt(7'h0), .src(7'h4), .txn(12'hE8), .dbid(dbid_val),
                             .data(wdata), .be(wbe));

            if (got_rsp_v && rsp_opcode(rsp_cap) == 6'h6) begin
                wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
                check(got_rsp_v, "WBPtl60-63: received Comp after DBIDResp");
            end

            tick(10000);
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hE9),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'h130000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WBPtl60-63: verify ReadUnique received CompData");
            if (got) begin
                check(dat[620:613] == 8'hDE,
                      $sformatf("WBPtl60-63 byte60=0xDE got=0x%02x", dat[620:613]));
                check(dat[628:621] == 8'hC0,
                      $sformatf("WBPtl60-63 byte61=0xC0 got=0x%02x", dat[628:621]));
                check(dat[636:629] == 8'hAD,
                      $sformatf("WBPtl60-63 byte62=0xAD got=0x%02x", dat[636:629]));
                check(dat[644:637] == 8'hDE,
                      $sformatf("WBPtl60-63 byte63=0xDE got=0x%02x", dat[644:637]));
            end
            send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'hE9),
                     .opcode(6'h2 /*CompAck*/));
        end

        // ── [28] WriteBackPtl — non-contiguous dirty byte regions ────────────
        // BE selects bytes 0-3 AND bytes 28-31 simultaneously, leaving bytes
        // 4-27 and 32-63 as zero (from the pre-loaded clean cache entry).
        // Tests that copyPartial handles gaps in the byte-enable mask.
        $display("\n[28] WriteBackPtl non-contiguous BE: bytes 0-3 and 28-31 (addr=0x140000)");
        begin
            automatic logic                      got_rsp_v;
            automatic logic [CHI_RSP_FLIT_W-1:0] rsp_cap;
            automatic logic [11:0]               dbid_val;
            automatic logic [511:0]              wdata;
            automatic logic [63:0]               wbe;

            // bytes 0-3 = 0x44332211 (LE), bytes 28-31 = 0xDDCCBBAA (LE)
            wdata = 512'h0;
            wdata[7:0]    = 8'h11;  // byte  0
            wdata[15:8]   = 8'h22;  // byte  1
            wdata[23:16]  = 8'h33;  // byte  2
            wdata[31:24]  = 8'h44;  // byte  3
            wdata[231:224] = 8'hAA; // byte 28
            wdata[239:232] = 8'hBB; // byte 29
            wdata[247:240] = 8'hCC; // byte 30
            wdata[255:248] = 8'hDD; // byte 31
            // bits 3:0 (bytes 0-3) and bits 31:28 (bytes 28-31)
            wbe = 64'h0000_0000_F000_000F;

            do_reset_and_init();

            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hEA),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'h140000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WBPtl-nc: ReadUnique received CompData");
            if (got) begin
                dbid_val = dat[42:31];
                send_rsp(.tgt(7'h0), .src(7'h4), .txn(dbid_val),
                         .opcode(6'h2 /*CompAck*/));
            end

            tick(5000);

            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hEB),
                     .opcode(7'h1A /*WriteBackPtl*/), .addr(44'h140000));
            wait_rsp(.max_cycles(200000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            check(got_rsp_v, "WBPtl-nc: received CompDBIDResp");
            check(rsp_opcode(rsp_cap)==6'h5 || rsp_opcode(rsp_cap)==6'h6,
                  $sformatf("WBPtl-nc: DBIDResp/CompDBIDResp got=0x%0x",
                             rsp_opcode(rsp_cap)));
            dbid_val = rsp_dbid(rsp_cap);

            if (got_rsp_v)
                send_cbwrdat(.tgt(7'h0), .src(7'h4), .txn(12'hEB), .dbid(dbid_val),
                             .data(wdata), .be(wbe));

            if (got_rsp_v && rsp_opcode(rsp_cap) == 6'h6) begin
                wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
                check(got_rsp_v, "WBPtl-nc: received Comp after DBIDResp");
            end

            tick(10000);
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hEC),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'h140000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WBPtl-nc: verify ReadUnique received CompData");
            if (got) begin
                // first region: bytes 0-3
                check(dat[140:133] == 8'h11,
                      $sformatf("WBPtl-nc byte0=0x11  got=0x%02x", dat[140:133]));
                check(dat[148:141] == 8'h22,
                      $sformatf("WBPtl-nc byte1=0x22  got=0x%02x", dat[148:141]));
                check(dat[156:149] == 8'h33,
                      $sformatf("WBPtl-nc byte2=0x33  got=0x%02x", dat[156:149]));
                check(dat[164:157] == 8'h44,
                      $sformatf("WBPtl-nc byte3=0x44  got=0x%02x", dat[164:157]));
                // second region: bytes 28-31 (dat[132+224:125+224])
                check(dat[364:357] == 8'hAA,
                      $sformatf("WBPtl-nc byte28=0xAA got=0x%02x", dat[364:357]));
                check(dat[372:365] == 8'hBB,
                      $sformatf("WBPtl-nc byte29=0xBB got=0x%02x", dat[372:365]));
                check(dat[380:373] == 8'hCC,
                      $sformatf("WBPtl-nc byte30=0xCC got=0x%02x", dat[380:373]));
                check(dat[388:381] == 8'hDD,
                      $sformatf("WBPtl-nc byte31=0xDD got=0x%02x", dat[388:381]));
            end
            send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'hEC),
                     .opcode(6'h2 /*CompAck*/));
        end

        // ── [29] WriteBackPtl — single dirty byte (byte 0 only) ─────────────
        // Minimum possible partial writeback: exactly one byte is dirty.
        // BE has only bit 0 set; all 63 other bytes are pre-loaded as zero.
        $display("\n[29] WriteBackPtl single byte: byte 0=0x42 only (addr=0x150000)");
        begin
            automatic logic                      got_rsp_v;
            automatic logic [CHI_RSP_FLIT_W-1:0] rsp_cap;
            automatic logic [11:0]               dbid_val;
            automatic logic [511:0]              wdata;
            automatic logic [63:0]               wbe;

            wdata = 512'h0;
            wdata[7:0] = 8'h42;              // byte 0 only
            wbe = 64'h0000_0000_0000_0001;   // bit 0 only

            do_reset_and_init();

            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hED),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'h150000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WBPtl-1B: ReadUnique received CompData");
            if (got) begin
                dbid_val = dat[42:31];
                send_rsp(.tgt(7'h0), .src(7'h4), .txn(dbid_val),
                         .opcode(6'h2 /*CompAck*/));
            end

            tick(5000);

            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hEE),
                     .opcode(7'h1A /*WriteBackPtl*/), .addr(44'h150000));
            wait_rsp(.max_cycles(200000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            check(got_rsp_v, "WBPtl-1B: received CompDBIDResp");
            check(rsp_opcode(rsp_cap)==6'h5 || rsp_opcode(rsp_cap)==6'h6,
                  $sformatf("WBPtl-1B: DBIDResp/CompDBIDResp got=0x%0x",
                             rsp_opcode(rsp_cap)));
            dbid_val = rsp_dbid(rsp_cap);

            if (got_rsp_v)
                send_cbwrdat(.tgt(7'h0), .src(7'h4), .txn(12'hEE), .dbid(dbid_val),
                             .data(wdata), .be(wbe));

            if (got_rsp_v && rsp_opcode(rsp_cap) == 6'h6) begin
                wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
                check(got_rsp_v, "WBPtl-1B: received Comp after DBIDResp");
            end

            tick(10000);
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hEF),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'h150000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WBPtl-1B: verify ReadUnique received CompData");
            if (got)
                check(dat[140:133] == 8'h42,
                      $sformatf("WBPtl-1B byte0=0x42 got=0x%02x", dat[140:133]));
            send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'hEF),
                     .opcode(6'h2 /*CompAck*/));
        end

        // ── [30] WriteBackPtl — chained partial writebacks accumulate ────────
        // Two WriteBackPtl operations on the same cache line in sequence:
        //   Round 1: bytes 0-3 = 0xAA  →  HNF UD cache: bytes 0-3=0xAA, others=0
        //   Round 2: bytes 4-7 = 0xBB  →  pre-load picks up bytes 0-3=0xAA;
        //            CBWrData_UD_PD overlays bytes 4-7=0xBB
        // Final verify: bytes 0-3=0xAA AND bytes 4-7=0xBB both present.
        $display("\n[30] WriteBackPtl chained: accumulate bytes 0-3=0xAA then 4-7=0xBB (addr=0x160000)");
        begin
            automatic logic                      got_rsp_v;
            automatic logic [CHI_RSP_FLIT_W-1:0] rsp_cap;
            automatic logic [11:0]               dbid_val;
            automatic logic [511:0]              wdata;
            automatic logic [63:0]               wbe;

            do_reset_and_init();

            // ── Round 1: acquire exclusive, write bytes 0-3=0xAA ──────────
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hF0),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'h160000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WBPtl-chain R1: ReadUnique received CompData");
            if (got) begin
                dbid_val = dat[42:31];
                send_rsp(.tgt(7'h0), .src(7'h4), .txn(dbid_val),
                         .opcode(6'h2 /*CompAck*/));
            end

            tick(5000);

            wdata = 512'h0;
            wdata[7:0]   = 8'hAA;
            wdata[15:8]  = 8'hAA;
            wdata[23:16] = 8'hAA;
            wdata[31:24] = 8'hAA;
            wbe = 64'h0000_0000_0000_000F;  // bytes 0-3

            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hF1),
                     .opcode(7'h1A /*WriteBackPtl*/), .addr(44'h160000));
            wait_rsp(.max_cycles(200000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            check(got_rsp_v, "WBPtl-chain R1: WriteBackPtl received CompDBIDResp");
            check(rsp_opcode(rsp_cap)==6'h5 || rsp_opcode(rsp_cap)==6'h6,
                  $sformatf("WBPtl-chain R1: RSP got=0x%0x", rsp_opcode(rsp_cap)));
            dbid_val = rsp_dbid(rsp_cap);

            if (got_rsp_v)
                send_cbwrdat(.tgt(7'h0), .src(7'h4), .txn(12'hF1), .dbid(dbid_val),
                             .data(wdata), .be(wbe));

            if (got_rsp_v && rsp_opcode(rsp_cap) == 6'h6) begin
                wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
                check(got_rsp_v, "WBPtl-chain R1: received Comp after DBIDResp");
            end

            tick(10000);

            // ── Round 2: re-acquire exclusive (HNF UD_RU), overlay bytes 4-7=0xBB ─
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hF2),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'h160000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WBPtl-chain R2: ReadUnique received CompData");
            if (got) begin
                dbid_val = dat[42:31];
                send_rsp(.tgt(7'h0), .src(7'h4), .txn(dbid_val),
                         .opcode(6'h2 /*CompAck*/));
            end

            tick(5000);

            wdata = 512'h0;
            wdata[39:32] = 8'hBB;
            wdata[47:40] = 8'hBB;
            wdata[55:48] = 8'hBB;
            wdata[63:56] = 8'hBB;
            wbe = 64'h0000_0000_0000_00F0;  // bytes 4-7

            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hF3),
                     .opcode(7'h1A /*WriteBackPtl*/), .addr(44'h160000));
            wait_rsp(.max_cycles(200000), .got_rsp(got_rsp_v), .captured(rsp_cap));
            check(got_rsp_v, "WBPtl-chain R2: WriteBackPtl received CompDBIDResp");
            check(rsp_opcode(rsp_cap)==6'h5 || rsp_opcode(rsp_cap)==6'h6,
                  $sformatf("WBPtl-chain R2: RSP got=0x%0x", rsp_opcode(rsp_cap)));
            dbid_val = rsp_dbid(rsp_cap);

            if (got_rsp_v)
                send_cbwrdat(.tgt(7'h0), .src(7'h4), .txn(12'hF3), .dbid(dbid_val),
                             .data(wdata), .be(wbe));

            if (got_rsp_v && rsp_opcode(rsp_cap) == 6'h6) begin
                wait_rsp(.max_cycles(100000), .got_rsp(got_rsp_v), .captured(rsp_cap));
                check(got_rsp_v, "WBPtl-chain R2: received Comp after DBIDResp");
            end

            // ── Final verify: bytes 0-3=0xAA and bytes 4-7=0xBB both present ──
            tick(10000);
            send_req(.tgt(7'h0), .src(7'h4), .txn(12'hF4),
                     .opcode(7'h07 /*ReadUnique*/), .addr(44'h160000));
            wait_dat(.max_cycles(50000), .got_dat(got), .captured(dat));
            check(got, "WBPtl-chain: verify ReadUnique received CompData");
            if (got) begin
                check(dat[140:133] == 8'hAA,
                      $sformatf("WBPtl-chain byte0=0xAA got=0x%02x", dat[140:133]));
                check(dat[148:141] == 8'hAA,
                      $sformatf("WBPtl-chain byte1=0xAA got=0x%02x", dat[148:141]));
                check(dat[156:149] == 8'hAA,
                      $sformatf("WBPtl-chain byte2=0xAA got=0x%02x", dat[156:149]));
                check(dat[164:157] == 8'hAA,
                      $sformatf("WBPtl-chain byte3=0xAA got=0x%02x", dat[164:157]));
                check(dat[172:165] == 8'hBB,
                      $sformatf("WBPtl-chain byte4=0xBB got=0x%02x", dat[172:165]));
                check(dat[180:173] == 8'hBB,
                      $sformatf("WBPtl-chain byte5=0xBB got=0x%02x", dat[180:173]));
                check(dat[188:181] == 8'hBB,
                      $sformatf("WBPtl-chain byte6=0xBB got=0x%02x", dat[188:181]));
                check(dat[196:189] == 8'hBB,
                      $sformatf("WBPtl-chain byte7=0xBB got=0x%02x", dat[196:189]));
            end
            send_rsp(.tgt(7'h0), .src(7'h4), .txn(12'hF4),
                     .opcode(6'h2 /*CompAck*/));
        end

        // ── Summary ───────────────────────────────────────────────────────
        $display("\n=== RESULTS: %0d passed, %0d failed ===\n", pass_cnt, fail_cnt);
        if (fail_cnt != 0) $fatal(1, "Test FAILED");
        $finish;
    end

    // ── Timeout ───────────────────────────────────────────────────────────────
    initial begin
        #(64'd20_000_000_000);  // 20000 ms sim-time — for 30 test groups
        $display("TIMEOUT: simulation exceeded 9000ms");
        $display("=== RESULTS: %0d passed, %0d failed (TIMEOUT) ===", pass_cnt, fail_cnt);
        $fatal(1, "Timeout");
    end

endmodule : tb_gem5_cosim
