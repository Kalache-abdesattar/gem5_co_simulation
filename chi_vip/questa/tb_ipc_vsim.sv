// tb_ipc_vsim.sv
//
// Self-checking SV testbench for questa_top_ipc_wrap.
//
// Runs without a live gem5 process.  A small shm_create helper must be
// started before vsim so that questa_top_ipc::start_of_simulation() can
// attach to the shared-memory segment (it only spins until attach succeeds;
// it does NOT require gem5 to be processing transactions).
//
// Tests (pin-level, observable without gem5 traffic):
//   [1]  Reset: all TX L-credit outputs low while rstn=0
//   [2]  Init credits: TXREQLCRDV pulses CHI_INIT_LCRD=15 times after reset
//   [3]  Init credits: TXRSPLCRDV and TXDATLCRDV also pulse 15 times
//   [4]  No extra credit pulse after init phase (credit counter exhausted)
//   [5]  txreq_lcrdv returns in the same cycle that txreq_flitv=1 is sampled
//   [6]  txrsp_lcrdv returns in the same cycle that txrsp_flitv=1 is sampled
//   [7]  txdat_lcrdv returns in the same cycle that txdat_flitv=1 is sampled
//   [8]  RX outputs remain low while the corresponding lcrdv input is 0
//   [9]  questa_ps advances monotonically (read via shm, checked in SC domain)
//
// Tests [8]-[9] require gem5 to inject outbound flits; they will SKIP with a
// timeout note when running without a gem5 process.
//
// Compile and run (from chi_vip/ directory):
//   # 1. Start shm holder in background
//   questa_build/shm_create &
//   SHM_PID=$!
//
//   # 2. Compile SV testbench
//   PATH=$QUESTA/bin:$PATH vlog -work questa_build/work -sv \
//       questa/questa_top_ipc_wrap.sv questa/tb_ipc_vsim.sv
//
//   # 3. Run
//   PATH=$QUESTA/bin:$PATH vsim -c -sc -lib questa_build/work \
//       questa_top_ipc_wrap tb_ipc_vsim \
//       -do "run -all; quit"
//
//   kill $SHM_PID

`timescale 1ps/1ps

module tb_ipc_vsim
    import chi_ipc_pkg::*;
();

    // ── Clock ─────────────────────────────────────────────────────────────────
    logic clk = 0;
    always #500 clk = ~clk;  // 1000ps period

    // ── Reset ─────────────────────────────────────────────────────────────────
    logic rstn = 0;

    // ── TX channel inputs (DUT → HN-F proxy) ─────────────────────────────────
    logic                         txreq_flitv    = 0;
    logic                         txreq_flitpend = 0;
    logic [CHI_REQ_FLIT_W-1:0]    txreq_flit     = '0;
    logic                         txrsp_flitv    = 0;
    logic                         txrsp_flitpend = 0;
    logic [CHI_RSP_FLIT_W-1:0]    txrsp_flit     = '0;
    logic                         txdat_flitv    = 0;
    logic                         txdat_flitpend = 0;
    logic [CHI_DAT_FLIT_W-1:0]    txdat_flit     = '0;

    // ── RX credit inputs (DUT grants credits to HN-F proxy) ──────────────────
    logic rxrsp_lcrdv = 0;
    logic rxdat_lcrdv = 0;
    logic rxsnp_lcrdv = 0;

    // ── Outputs from questa_top_ipc_wrap ─────────────────────────────────────
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

    // ── DUT instantiation ─────────────────────────────────────────────────────
    questa_top_ipc_wrap u_dut (
        .clk             (clk),
        .rstn            (rstn),
        .txreq_flitv     (txreq_flitv),
        .txreq_flitpend  (txreq_flitpend),
        .txreq_flit      (txreq_flit),
        .txreq_lcrdv     (txreq_lcrdv),
        .txrsp_flitv     (txrsp_flitv),
        .txrsp_flitpend  (txrsp_flitpend),
        .txrsp_flit      (txrsp_flit),
        .txrsp_lcrdv     (txrsp_lcrdv),
        .txdat_flitv     (txdat_flitv),
        .txdat_flitpend  (txdat_flitpend),
        .txdat_flit      (txdat_flit),
        .txdat_lcrdv     (txdat_lcrdv),
        .rxrsp_flitv     (rxrsp_flitv),
        .rxrsp_flitpend  (rxrsp_flitpend),
        .rxrsp_flit      (rxrsp_flit),
        .rxrsp_lcrdv     (rxrsp_lcrdv),
        .rxdat_flitv     (rxdat_flitv),
        .rxdat_flitpend  (rxdat_flitpend),
        .rxdat_flit      (rxdat_flit),
        .rxdat_lcrdv     (rxdat_lcrdv),
        .rxsnp_flitv     (rxsnp_flitv),
        .rxsnp_flitpend  (rxsnp_flitpend),
        .rxsnp_flit      (rxsnp_flit),
        .rxsnp_lcrdv     (rxsnp_lcrdv)
    );

    // ── Test infrastructure ───────────────────────────────────────────────────
    int pass_cnt = 0;
    int fail_cnt = 0;

    task automatic posedge_clk;
        @(posedge clk);
        #1;  // small delta so outputs have settled after posedge
    endtask

    task automatic check(input logic cond, input string msg);
        if (cond) begin
            $display("  PASS: %s", msg);
            pass_cnt++;
        end else begin
            $display("  FAIL: %s", msg);
            fail_cnt++;
        end
    endtask

    task automatic tick(input int n);
        repeat(n) posedge_clk;
    endtask

    // ── Main test sequence ────────────────────────────────────────────────────
    initial begin
        // Wait a few cycles for SystemC to elaborate and spin on shm attach
        #5000;

        // ── [1] Reset: all TX L-credit outputs low during rstn=0 ──────────
        $display("\n[1] Reset: all TX L-credit outputs low during rstn=0");
        rstn = 0;
        tick(5);
        check(txreq_lcrdv === 0, "txreq_lcrdv=0 during reset");
        check(txrsp_lcrdv === 0, "txrsp_lcrdv=0 during reset");
        check(txdat_lcrdv === 0, "txdat_lcrdv=0 during reset");
        check(rxrsp_flitv === 0, "rxrsp_flitv=0 during reset");
        check(rxdat_flitv === 0, "rxdat_flitv=0 during reset");
        check(rxsnp_flitv === 0, "rxsnp_flitv=0 during reset");

        // ── [2] Init credits: TXREQLCRDV pulses 15 times ──────────────────
        $display("\n[2] Init credits: txreq_lcrdv pulses CHI_INIT_LCRD=15 times");
        begin
            automatic int req_cnt = 0;
            rstn = 1;
            repeat (80) begin
                posedge_clk;
                if (txreq_lcrdv === 1) req_cnt++;
            end
            check(req_cnt === 15, $sformatf("txreq_lcrdv pulsed exactly 15 times (got %0d)", req_cnt));
        end

        // ── [3] Init credits: TXRSPLCRDV and TXDATLCRDV also 15 times ─────
        $display("\n[3] Init credits: txrsp_lcrdv and txdat_lcrdv pulse 15 times");
        begin
            automatic int rsp_cnt = 0;
            automatic int dat_cnt = 0;
            // Reset and re-count (second reset cycle)
            rstn = 0; tick(3);
            rsp_cnt = 0; dat_cnt = 0;
            rstn = 1;
            repeat (80) begin
                posedge_clk;
                if (txrsp_lcrdv === 1) rsp_cnt++;
                if (txdat_lcrdv === 1) dat_cnt++;
            end
            check(rsp_cnt === 15, $sformatf("txrsp_lcrdv pulsed exactly 15 (got %0d)", rsp_cnt));
            check(dat_cnt === 15, $sformatf("txdat_lcrdv pulsed exactly 15 (got %0d)", dat_cnt));
        end

        // ── [4] No extra credit after init phase ──────────────────────────
        $display("\n[4] No extra credit after init phase (credits exhausted)");
        begin
            automatic int extra_req = 0;
            automatic int extra_rsp = 0;
            automatic int extra_dat = 0;
            repeat (20) begin
                posedge_clk;
                if (txreq_lcrdv === 1) extra_req++;
                if (txrsp_lcrdv === 1) extra_rsp++;
                if (txdat_lcrdv === 1) extra_dat++;
            end
            check(extra_req === 0, "txreq_lcrdv=0 after init phase");
            check(extra_rsp === 0, "txrsp_lcrdv=0 after init phase");
            check(extra_dat === 0, "txdat_lcrdv=0 after init phase");
        end

        // ── [5] txreq_lcrdv returns same cycle as txreq_flitv=1 ──────────
        $display("\n[5] txreq_lcrdv returned same posedge txreq_flitv=1 sampled");
        begin
            // Re-init: fresh reset so credits are available
            rstn = 0; tick(3); rstn = 1; tick(20);  // burn the 15 init credits silently
            // Re-reset to get credits back
            rstn = 0; tick(3); rstn = 1; tick(20);
            txreq_flit[6:0]  = 7'h01;  // opcode ReadShared in bits [45:39] — just non-zero
            txreq_flitv      = 1;
            posedge_clk;
            // txreq_lcrdv must be high in the same posedge
            check(txreq_lcrdv === 1, "txreq_lcrdv=1 in same cycle as flitv=1");
            txreq_flitv = 0;
            tick(1);
            check(txreq_lcrdv === 0, "txreq_lcrdv=0 the cycle after flitv de-asserted");
        end

        // ── [6] txrsp_lcrdv returned same cycle ───────────────────────────
        $display("\n[6] txrsp_lcrdv returned same posedge txrsp_flitv=1 sampled");
        begin
            rstn = 0; tick(3); rstn = 1; tick(20);
            txrsp_flit[5:0] = 6'h24;  // CompAck opcode
            txrsp_flitv     = 1;
            posedge_clk;
            check(txrsp_lcrdv === 1, "txrsp_lcrdv=1 same cycle as flitv=1");
            txrsp_flitv = 0;
            tick(1);
            check(txrsp_lcrdv === 0, "txrsp_lcrdv=0 after de-assert");
        end

        // ── [7] txdat_lcrdv returned same cycle ───────────────────────────
        $display("\n[7] txdat_lcrdv returned same posedge txdat_flitv=1 sampled");
        begin
            rstn = 0; tick(3); rstn = 1; tick(20);
            txdat_flit[3:0] = 4'h2;  // CopyBackWrData opcode [21:18]
            txdat_flitv     = 1;
            posedge_clk;
            check(txdat_lcrdv === 1, "txdat_lcrdv=1 same cycle as flitv=1");
            txdat_flitv = 0;
            tick(1);
            check(txdat_lcrdv === 0, "txdat_lcrdv=0 after de-assert");
        end

        // ── [8] RX outputs low when lcrdv input=0 ─────────────────────────
        $display("\n[8] RX outputs stay low when RX lcrdv inputs=0 (no gem5 traffic)");
        begin
            rstn = 0; tick(3); rstn = 1; tick(10);
            rxrsp_lcrdv = 0; rxdat_lcrdv = 0; rxsnp_lcrdv = 0;
            tick(10);
            check(rxrsp_flitv === 0, "rxrsp_flitv=0 (no credits granted, no gem5 traffic)");
            check(rxdat_flitv === 0, "rxdat_flitv=0 (no credits granted, no gem5 traffic)");
            check(rxsnp_flitv === 0, "rxsnp_flitv=0 (no credits granted, no gem5 traffic)");
        end

        // ── Done ──────────────────────────────────────────────────────────
        $display("\n=== RESULTS: %0d passed, %0d failed ===\n", pass_cnt, fail_cnt);
        if (fail_cnt != 0) $fatal(1, "Test FAILED");
        $finish;
    end

    // ── Timeout watchdog ─────────────────────────────────────────────────────
    initial begin
        #5_000_000;  // 5 ms sim-time
        $display("TIMEOUT: simulation exceeded 5ms — possible shm hang");
        $fatal(1, "Timeout");
    end

endmodule : tb_ipc_vsim
