/* lrsc_stress.S — RISC-V LR/SC exclusive-monitor stress for gem5 CHI SE mode
 *
 * All NUM_CORES cores spin on a counter barrier before starting the stress
 * loop, so the LR/SC traffic arrives at the HN-F as a simultaneous burst
 * rather than one core racing ahead of the others.
 *
 * Barrier protocol (two-cache-line sense counter):
 *   1. Each core increments g_cnt with lr.w/sc.w (retry on sc failure).
 *      g_cnt is never polled — one CAS per core, then abandon the line.
 *   2. The last core (result == NUM_CORES) stores 1 to g_go with a plain
 *      sw, then fence w,w.
 *   3. All other cores poll g_go (plain lw) on a separate cache line.
 *   4. A fence r,r after the poll ensures barrier completion is globally
 *      visible before the stress loop.
 *
 * Using two separate cache lines for g_cnt and g_go avoids the pathological
 * case where sc_lock_enabled causes exponential backoff on the same line
 * that all cores are spinning on.  amoadd.w is intentionally avoided: in
 * gem5 RISC-V it goes through LoadLocked/StoreCond and therefore inherits
 * sc_lock_multiplier backoff when sc_lock_enabled=True.
 *
 * Must match --num-cores in config.py (default: 16).
 *
 * Output per core:
 *   lrsc: ok=NNNNN fail=NNNNN
 *
 * Build:
 *   riscv64-unknown-elf-gcc -static -march=rv64imafd -mabi=lp64d \
 *       -nostdlib -g -o lrsc_stress lrsc_stress.S
 */

    .set    NUM_CORES, 4        /* must match --num-cores in config.py    */
    .set    N_ITER,    50000

    /* ------------------------------------------------------------------ */
    /*  Read-only strings                                                  */
    /* ------------------------------------------------------------------ */
    .section .rodata
msg_start:  .ascii "lrsc: ok="
            .set   msg_start_len, . - msg_start
msg_sep:    .ascii " fail="
            .set   msg_sep_len,   . - msg_sep
msg_nl:     .ascii "\n"
            .set   msg_nl_len,    . - msg_nl

    /* ------------------------------------------------------------------ */
    /*  Shared data (all on separate cache lines to avoid false sharing)  */
    /* ------------------------------------------------------------------ */
    .section .data

    .align  6                           /* 64-byte cache-line alignment   */
g_cnt:
    .word   0                           /* arrival counter — CAS only, never polled */

    .align  6
g_go:
    .word   0                           /* go flag — written once, polled by waiters */

    .align  6
g_shared:
    .word   0                           /* the contested cache line       */

    /* ------------------------------------------------------------------ */
    /*  Entry point                                                        */
    /* ------------------------------------------------------------------ */
    .section .text
    .globl _start
_start:
    /* Initialize gp so the linker's GP-relative relaxations work.
     * .option norelax prevents the assembler from turning this la into
     * another gp-relative access (which would be circular). */
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop

    /* ------------------------------------------------------------------
     * Barrier — stage 1: atomically increment g_cnt with lr.w/sc.w.
     * Retry only on sc.w failure; never spin-poll g_cnt.
     * ------------------------------------------------------------------ */
    la      t0, g_cnt
    la      t1, g_go

.Lbarr_inc:
    lr.w    t2, (t0)                    /* exclusive load of counter      */
    addi    t2, t2, 1
    sc.w    t3, t2, (t0)               /* try to write incremented value */
    bnez    t3, .Lbarr_inc             /* retry if sc failed             */

    /* t2 now holds the value we successfully stored (our arrival index)  */
    li      t4, NUM_CORES
    beq     t2, t4, .Lbarr_signal      /* last core → write go flag      */

    /* ------------------------------------------------------------------
     * Barrier — stage 2 (non-last cores): poll g_go on a separate line.
     * Plain lw — never touches g_cnt again.
     * ------------------------------------------------------------------ */
.Lbarr_poll:
    lw      t5, 0(t1)
    beqz    t5, .Lbarr_poll
    j       .Lbarr_done

    /* ------------------------------------------------------------------
     * Barrier — stage 2 (last core): signal all waiters.
     * ------------------------------------------------------------------ */
.Lbarr_signal:
    li      t5, 1
    sw      t5, 0(t1)                  /* release g_go                   */
    fence   w, w                       /* ensure store is globally visible */

.Lbarr_done:
    fence   r, r                       /* all prior reads complete        */

    /* ------------------------------------------------------------------
     * Stress loop
     * ------------------------------------------------------------------ */
    la      a0, g_shared
    li      a1, N_ITER
    li      a2, 0                       /* value written by SC            */
    li      s0, 0                       /* sc_ok  counter                 */
    li      s1, 0                       /* sc_fail counter                */

.Lloop:
    beqz    a1, .Ldone

    lr.w    t0, (a0)                    /* ReadUnique(excl=1) → HN-F      */
    sc.w    t1, a2, (a0)               /* SC: t1=0 success, t1=1 failure */

    bnez    t1, .Lfail
    addi    s0, s0, 1
    j       .Lnext
.Lfail:
    addi    s1, s1, 1
.Lnext:
    addi    a2, a2, 1
    addi    a1, a1, -1
    j       .Lloop

    /* ------------------------------------------------------------------
     * Print result and exit
     * ------------------------------------------------------------------ */
.Ldone:
    li      a7, 64
    li      a0, 1
    la      a1, msg_start
    li      a2, msg_start_len
    ecall

    mv      a0, s0
    call    print_uint

    li      a7, 64
    li      a0, 1
    la      a1, msg_sep
    li      a2, msg_sep_len
    ecall

    mv      a0, s1
    call    print_uint

    li      a7, 64
    li      a0, 1
    la      a1, msg_nl
    li      a2, msg_nl_len
    ecall

    li      a7, 93
    li      a0, 0
    ecall


    /* ------------------------------------------------------------------ */
    /*  print_uint: write a0 as decimal to stdout                         */
    /*  Clobbers: a0-a7, t0-t6, a4-a5                                    */
    /* ------------------------------------------------------------------ */
print_uint:
    addi    sp, sp, -32
    mv      t0, sp                      /* digit buffer start             */
    li      t1, 0                       /* digit count                    */
    li      t2, 10

    bnez    a0, .Lpu_digits
    li      t3, 0x30
    sb      t3, 0(t0)
    li      t1, 1
    j       .Lpu_reverse

.Lpu_digits:
    beqz    a0, .Lpu_reverse
    remu    t3, a0, t2
    divu    a0, a0, t2
    addi    t3, t3, 0x30
    add     t4, t0, t1
    sb      t3, 0(t4)
    addi    t1, t1, 1
    j       .Lpu_digits

.Lpu_reverse:
    li      t3, 0
    addi    t4, t1, -1
.Lpu_rev:
    bge     t3, t4, .Lpu_write
    add     t5, t0, t3
    lb      t6, 0(t5)
    add     a4, t0, t4
    lb      a5, 0(a4)
    sb      a5, 0(t5)
    sb      t6, 0(a4)
    addi    t3, t3, 1
    addi    t4, t4, -1
    j       .Lpu_rev

.Lpu_write:
    li      a7, 64
    li      a0, 1
    mv      a1, t0
    mv      a2, t1
    ecall

    addi    sp, sp, 32
    ret
