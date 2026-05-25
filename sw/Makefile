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

TESTS   := test_01_basic_rw test_02_atomic test_03_stride \
           test_04_upgrade test_05_evict test_06_amo_ops \
           test_07_lrsc_stress test_08_mixed_coherency \
           test_09_amo_minmax test_10_amo_logic \
           test_11_word_atomics test_12_partial_store \
           test_13_fill_line test_14_seq_rw \
           test_15_rd_modify_wr test_16_copy
BINS    := $(TESTS:%=build/%.bin)
ELFS    := $(TESTS:%=build/%.elf)

# Multi-hart (2-core) tests — compiled with -DPITON_NUMTILES=2
MT_TESTS  := mt_test_01_shared_rw mt_test_02_atomic_counter \
             mt_test_02b_lrsc_counter \
             mt_test_03_spinlock mt_test_03b_spinlock_tas \
             mt_test_04_producer_consumer mt_test_05_barrier_stress \
             mt_test_06_false_sharing mt_test_07_parallel_sum
MT_CFLAGS := $(CFLAGS) -DPITON_NUMTILES=2
MT_WHISPER_OPTS := $(WHISPER_OPTS) --harts 2 --deterministic 1000:1000

.PRECIOUS: $(ELFS)

TRACES  := $(TESTS:%=build/%.trace)

# Benchmark suite (STREAM, vvadd, matmul, SpMV, GEMM, Jacobi-2D)
BM_TESTS  := bm_stream bm_vvadd bm_matmul bm_spmv bm_gemm bm_jacobi2d
BM_CFLAGS := $(CFLAGS) -DPITON_NUMTILES=2

.PHONY: all clean load disasm trace mt mt_trace mt_disasm bm bm_disasm

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

clean:
	rm -rf build
