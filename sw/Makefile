RV_PREFIX := $(HOME)/fedora/rvgcc/bin/riscv64-unknown-elf-
WHISPER   := $(HOME)/fedora/whisper/build-Linux/whisper
WHISPER_OPTS := --isa rv64imafdc --xlen 64
CC      := $(RV_PREFIX)gcc
OBJCOPY := $(RV_PREFIX)objcopy
OBJDUMP := $(RV_PREFIX)objdump

INC     := $(CURDIR)/include
CRT_SRC := $(INC)/crt.S $(INC)/syscalls.c

CFLAGS  := -march=rv64imafdc -mabi=lp64d -mcmodel=medany \
           -O1 -g -static -nostdlib -nostartfiles \
           -I$(INC)
LDFLAGS := -T$(INC)/link.ld

# Hart count for multi-hart targets (mt, bm, litmus). Override on the command
# line: `make mt HARTS=4`. The value is forwarded to crt.S via PITON_NUMTILES,
# which gates how many harts proceed past _start.
HARTS   ?= 2

TESTS   := test_01_basic_rw test_02_atomic test_03_stride \
           test_04_upgrade test_05_evict test_06_amo_ops \
           test_07_lrsc_stress test_08_mixed_coherency \
           test_09_amo_minmax test_10_amo_logic \
           test_11_word_atomics test_12_partial_store \
           test_13_fill_line test_14_seq_rw \
           test_15_rd_modify_wr test_16_copy
BINS    := $(TESTS:%=build/%.bin)
ELFS    := $(TESTS:%=build/%.elf)

# Multi-hart (2-core) tests — compiled with -DPITON_NUMTILES=$(HARTS)
MT_TESTS  := mt_test_01_shared_rw mt_test_02_atomic_counter \
             mt_test_02b_lrsc_counter \
             mt_test_03_spinlock mt_test_03b_spinlock_tas \
             mt_test_04_producer_consumer mt_test_05_barrier_stress \
             mt_test_06_false_sharing mt_test_07_parallel_sum
MT_CFLAGS := $(CFLAGS) -DPITON_NUMTILES=$(HARTS)
MT_WHISPER_OPTS := $(WHISPER_OPTS) --harts $(HARTS) --deterministic 1000:1000

.PRECIOUS: $(ELFS) build/litmus_%.elf build/mt_%.elf build/bm_%.elf

TRACES  := $(TESTS:%=build/%.trace)

# Benchmark suite (STREAM, vvadd, matmul, SpMV, GEMM, Jacobi-2D)
BM_TESTS  := bm_stream bm_vvadd bm_matmul bm_spmv bm_gemm bm_jacobi2d
BM_CFLAGS := $(CFLAGS) -DPITON_NUMTILES=$(HARTS)

.PHONY: all clean load disasm trace mt mt_trace mt_disasm bm bm_disasm ds ds_disasm app app_disasm

all: $(BINS)

build:
	@mkdir -p build

build/%.elf: tests/%.c $(CRT_SRC) $(INC)/link.ld | build
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(CRT_SRC) -lgcc

build/%.bin: build/%.elf
	$(OBJCOPY) -O binary $< $@
	@size $<

# Copy a single test binary as the active dram_fw.bin
# Usage: make load TEST=test_01_basic_rw
load: build/$(TEST).bin
	cp $< ../chi_vip/dram_fw.bin
	@echo "Loaded $(TEST) as dram_fw.bin"

# Dump disassembly for inspection
disasm: $(TESTS:%=build/%.dis)

build/%.dis: build/%.elf
	$(OBJDUMP) -d -S $< > $@

trace: $(TRACES)

build/%.trace: build/%.elf
	$(WHISPER) $(WHISPER_OPTS) --logfile $@ -t $<

# ── Multi-hart rules ──────────────────────────────────────────────────────────
mt: $(MT_TESTS:%=build/%.elf) $(MT_TESTS:%=build/%.bin)

mt_trace: $(MT_TESTS:%=build/%.trace)

mt_disasm: $(MT_TESTS:%=build/%.dis)

build/mt_%.elf: tests/mt_%.c $(CRT_SRC) $(INC)/link.ld | build
	$(CC) $(MT_CFLAGS) $(LDFLAGS) -o $@ $< $(CRT_SRC) -lgcc

build/mt_%.bin: build/mt_%.elf
	$(OBJCOPY) -O binary $< $@
	@size $<

build/mt_%.trace: build/mt_%.elf
	$(WHISPER) $(MT_WHISPER_OPTS) --logfile $@ -t $<

build/mt_%.dis: build/mt_%.elf
	$(OBJDUMP) -d -S $< > $@

# ── Data-structure benchmark rules ───────────────────────────────────────────
# Concurrent data structures (stack/queue/deque/bst) on the 2-hart runtime.
# Built like mt_* (HARTS harts) but with tests/ds_*.c + include/conc.h.
DS_TESTS  := ds_stack ds_queue ds_deque ds_bst ds_list ds_hash ds_skiplist \
             ds_extbst ds_lazylist ds_rbtree ds_bsttk ds_harris ds_fraser \
             ds_citrus
DS_CFLAGS := $(CFLAGS) -DPITON_NUMTILES=$(HARTS)

ds: $(DS_TESTS:%=build/%.elf) $(DS_TESTS:%=build/%.bin)
ds_disasm: $(DS_TESTS:%=build/%.dis)

build/ds_%.elf: tests/ds_%.c $(INC)/conc.h $(CRT_SRC) $(INC)/link.ld | build
	$(CC) $(DS_CFLAGS) $(LDFLAGS) -o $@ $< $(CRT_SRC) -lgcc

build/ds_%.bin: build/ds_%.elf
	$(OBJCOPY) -O binary $< $@
	@size $<

build/ds_%.dis: build/ds_%.elf
	$(OBJDUMP) -d -S $< > $@

# ── Application/kernel benchmarks ─────────────────────────────────────────────
# Real application patterns the data-structure tests miss: ordered multi-lock
# (bank), atomic-counter contention (histogram), barrier data-parallel (reduce).
# Same 2-hart conc.h runtime as ds_*.
APP_TESTS  := app_bank app_histogram app_reduce
APP_CFLAGS := $(CFLAGS) -DPITON_NUMTILES=$(HARTS)

app: $(APP_TESTS:%=build/%.elf) $(APP_TESTS:%=build/%.bin)
app_disasm: $(APP_TESTS:%=build/%.dis)

build/app_%.elf: tests/app_%.c $(INC)/conc.h $(CRT_SRC) $(INC)/link.ld | build
	$(CC) $(APP_CFLAGS) $(LDFLAGS) -o $@ $< $(CRT_SRC) -lgcc

build/app_%.bin: build/app_%.elf
	$(OBJCOPY) -O binary $< $@
	@size $<

build/app_%.dis: build/app_%.elf
	$(OBJDUMP) -d -S $< > $@

# ── Benchmark rules ───────────────────────────────────────────────────────────
bm: $(BM_TESTS:%=build/%.elf) $(BM_TESTS:%=build/%.bin)

bm_disasm: $(BM_TESTS:%=build/%.dis)

build/bm_%.elf: tests/bm_%.c $(CRT_SRC) $(INC)/link.ld | build
	$(CC) $(BM_CFLAGS) $(LDFLAGS) -o $@ $< $(CRT_SRC) -lgcc

build/bm_%.bin: build/bm_%.elf
	$(OBJCOPY) -O binary $< $@
	@size $<

build/bm_%.dis: build/bm_%.elf
	$(OBJDUMP) -d -S $< > $@

# ── Litmus tests (litmus-tests-riscv submodule) ──────────────────────────────
# Memory-consistency litmus tests from https://github.com/litmus-tests/litmus-tests-riscv
# generated by the diy/herd toolset. Tests live in sw/litmus-tests-riscv/tests/
# as .litmus files and must be converted to baremetal C via the wrapper under
# sw/litmus/ before they can be built and run on the RTL.
#
# Workflow (see sw/litmus/README.md for details):
#   1. Pick a .litmus test (e.g. non-mixed-size/BASIC_2_THREAD/MP)
#   2. Convert to baremetal C: sw/litmus/port.sh <path-to.litmus> sw/litmus/ported/<name>.c
#   3. Build: make litmus TEST=<name> HARTS=2
#   4. Load:  make litmus_load TEST=<name>
LITMUS_DIR   := litmus/ported
LITMUS_TESTS := $(patsubst $(LITMUS_DIR)/%.c,%,$(wildcard $(LITMUS_DIR)/*.c))

litmus: $(LITMUS_TESTS:%=build/litmus_%.bin)

# Litmus tests use fence.i (zifencei) and AMO acq/rel suffixes; broaden -march.
LITMUS_CFLAGS := $(MT_CFLAGS) -march=rv64imafdc_zifencei -Isw/litmus

build/litmus_%.elf: $(LITMUS_DIR)/%.c litmus/harness.h $(CRT_SRC) $(INC)/link.ld | build
	$(CC) $(LITMUS_CFLAGS) $(LDFLAGS) -o $@ $< $(CRT_SRC) -lgcc

build/litmus_%.bin: build/litmus_%.elf
	$(OBJCOPY) -O binary $< $@
	@size $<

# Verbose dump: -d (disassemble text) -S (interleave source, requires -g)
# -l (source file:line annotations) -r (relocs) -t (symbol table) -h (section
# headers) -C (demangle) --wide (no column truncation) --source-comment=';'
# (prefix source lines so grep/sed can strip them).  ELFs are built with -g,
# so source-interleave works even at -O1.
build/litmus_%.dis: build/litmus_%.elf
	$(OBJDUMP) -d -S -l -r -t -h -C --wide --source-comment=';' \
	           --disassembler-options=numeric $< > $@

build/litmus_%.trace: build/litmus_%.elf
	$(WHISPER) $(MT_WHISPER_OPTS) --logfile $@ -t $<

litmus_disasm: $(LITMUS_TESTS:%=build/litmus_%.dis)
litmus_trace:  $(LITMUS_TESTS:%=build/litmus_%.trace)

# Load a single litmus test as the active dram_fw.bin
# Usage: make litmus_load TEST=<name>
litmus_load: build/litmus_$(TEST).bin
	cp $< ../chi_vip/dram_fw.bin
	@echo "Loaded litmus_$(TEST) as dram_fw.bin"

.PHONY: litmus litmus_disasm litmus_trace litmus_load

clean:
	rm -rf build
