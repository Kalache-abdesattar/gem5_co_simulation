GEM5_SRC  := $(CURDIR)/gem5_src
SHIM      := $(CURDIR)/chi_shim
LIBSOC    ?= $(HOME)/fedora/scc_install_questa
SC_ROOT   ?= $(HOME)/fedora/systemc

CXX       := g++
CXXFLAGS  := -std=c++17 -O2 -fPIC -Wall -Wno-unused-variable \
             -I$(SHIM)              \
             -I$(GEM5_SRC)          \
             -I$(LIBSOC)/include    \
             -I$(SC_ROOT)/src       \
             -Isc                   \
             -Iquesta               \
             -Iipc

SC_LIBS   := -L$(SC_ROOT)/build/src -lsystemc \
             -Wl,-rpath,$(SC_ROOT)/build/src

OBJS      := build/CHIGenericController.o \
             build/utils.o                \
             build/chi_vip_controller.o

QUESTA_OBJS := build/questa_top.o

# IPC objects used by libchi_ipc.so and libchi_questa_ipc.so
IPC_OBJS  := build/chi_ipc.o

TESTS     := build/smoke \
             build/questa_cosim_test \
             build/test_questa_cosim \
             build/test_questa_top \
             build/test_opcode_maps \
             build/test_pack_unpack \
             build/test_wakeup \
             build/test_multibeat \
             build/test_credit \
             build/test_rsp_types \
             build/test_dat_types \
             build/test_req_types \
             build/test_snp_types \
             build/test_concurrent \
             build/test_channels \
             build/test_edge_cases \
             build/test_ipc \
             build/test_questa_top_ipc

all: build/libchi_vip.so build/libchi_questa.so \
     build/libchi_ipc.so build/libchi_questa_ipc.so \
     $(TESTS)

build/CHIGenericController.o: $(GEM5_SRC)/CHIGenericController.cc
	mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/utils.o: $(GEM5_SRC)/utils.cc
	mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/chi_vip_controller.o: sc/chi_vip_controller.cc sc/chi_vip_controller.hh
	mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/libchi_vip.so: $(OBJS)
	$(CXX) -shared -o $@ $^ $(SC_LIBS)

build/questa_top.o: questa/questa_top.cc questa/questa_top.hh \
                    questa/chi_flit_cfg.hh questa/chi_flit_pack.hh \
                    sc/chi_vip_controller.hh
	mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/chi_ipc.o: ipc/chi_ipc.cc ipc/chi_ipc.hh
	mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

# libchi_ipc.so: POSIX shm IPC layer only (no SystemC; links -lrt)
build/libchi_ipc.so: $(IPC_OBJS)
	$(CXX) -shared -o $@ $^ -lrt

build/questa_top_ipc.o: questa/questa_top_ipc.cc questa/questa_top_ipc.hh \
                        questa/chi_flit_cfg.hh questa/chi_flit_pack.hh \
                        ipc/chi_ipc.hh
	mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

# libchi_questa_ipc.so: IPC questa_top (no CHI_VIP_Controller; links -lrt)
build/libchi_questa_ipc.so: build/questa_top_ipc.o build/libchi_ipc.so
	$(CXX) -shared -o $@ build/questa_top_ipc.o \
	    -L./build -lchi_ipc $(SC_LIBS) -lrt \
	    -Wl,-rpath,./build

build/libchi_questa.so: build/questa_top.o build/libchi_vip.so
	$(CXX) -shared -o $@ build/questa_top.o -L./build -lchi_vip $(SC_LIBS)

LINK_TEST = $(CXX) $(CXXFLAGS) $< -o $@ -L./build -lchi_vip $(SC_LIBS) -Wl,-rpath,./build

build/smoke: test/smoke.cc build/libchi_vip.so
	$(LINK_TEST)

build/test_questa_cosim: test/test_questa_cosim.cc build/libchi_questa.so
	$(CXX) $(CXXFLAGS) $< -o $@ -L./build -lchi_questa -lchi_vip $(SC_LIBS) -Wl,-rpath,./build

build/questa_cosim_test: questa/questa_cosim_top.cc build/libchi_questa.so
	$(CXX) $(CXXFLAGS) -DQUESTA_STANDALONE_TEST $< -o $@ -L./build -lchi_questa -lchi_vip $(SC_LIBS) -Wl,-rpath,./build

build/test_questa_top: test/test_questa_top.cc build/libchi_questa.so
	$(CXX) $(CXXFLAGS) $< -o $@ -L./build -lchi_questa -lchi_vip $(SC_LIBS) -Wl,-rpath,./build

build/test_opcode_maps: test/test_opcode_maps.cc build/libchi_vip.so
	$(LINK_TEST)

build/test_pack_unpack: test/test_pack_unpack.cc build/libchi_vip.so
	$(LINK_TEST)

build/test_wakeup: test/test_wakeup.cc build/libchi_vip.so
	$(LINK_TEST)

build/test_multibeat: test/test_multibeat.cc build/libchi_vip.so
	$(LINK_TEST)

build/test_credit: test/test_credit.cc build/libchi_vip.so
	$(LINK_TEST)

build/test_rsp_types: test/test_rsp_types.cc build/libchi_vip.so
	$(LINK_TEST)

build/test_dat_types: test/test_dat_types.cc build/libchi_vip.so
	$(LINK_TEST)

build/test_req_types: test/test_req_types.cc build/libchi_vip.so
	$(LINK_TEST)

build/test_snp_types: test/test_snp_types.cc build/libchi_vip.so
	$(LINK_TEST)

build/test_concurrent: test/test_concurrent.cc build/libchi_vip.so
	$(LINK_TEST)

build/test_channels: test/test_channels.cc build/libchi_vip.so
	$(LINK_TEST)

build/test_edge_cases: test/test_edge_cases.cc build/libchi_vip.so
	$(LINK_TEST)

# IPC tests: pure C++ (no SystemC), links only libchi_ipc.so + -lrt + -lpthread
build/test_ipc: test/test_ipc.cc build/libchi_ipc.so
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@ -L./build -lchi_ipc -lpthread -lrt \
	    -Wl,-rpath,./build

# questa_top_ipc integration tests: SystemC, links libchi_questa_ipc.so + libchi_ipc.so
build/test_questa_top_ipc: test/test_questa_top_ipc.cc \
                            build/libchi_questa_ipc.so build/libchi_ipc.so
	mkdir -p build
	$(CXX) $(CXXFLAGS) $< -o $@ \
	    -L./build -lchi_questa_ipc -lchi_ipc $(SC_LIBS) -lrt \
	    -Wl,-rpath,./build

RUN_ENV = LD_LIBRARY_PATH=./build:$(SC_ROOT)/build/src

run: $(TESTS)
	@echo ""; echo "━━━ smoke ━━━"
	@$(RUN_ENV) ./build/smoke
	@echo ""; echo "━━━ questa_cosim_test ━━━"
	@$(RUN_ENV) ./build/questa_cosim_test
	@echo ""; echo "━━━ test_questa_cosim ━━━"
	@$(RUN_ENV) ./build/test_questa_cosim
	@echo ""; echo "━━━ test_questa_top ━━━"
	@$(RUN_ENV) ./build/test_questa_top
	@echo ""; echo "━━━ test_opcode_maps ━━━"
	@$(RUN_ENV) ./build/test_opcode_maps
	@echo ""; echo "━━━ test_pack_unpack ━━━"
	@$(RUN_ENV) ./build/test_pack_unpack
	@echo ""; echo "━━━ test_wakeup ━━━"
	@$(RUN_ENV) ./build/test_wakeup
	@echo ""; echo "━━━ test_multibeat ━━━"
	@$(RUN_ENV) ./build/test_multibeat
	@echo ""; echo "━━━ test_credit ━━━"
	@$(RUN_ENV) ./build/test_credit
	@echo ""; echo "━━━ test_rsp_types ━━━"
	@$(RUN_ENV) ./build/test_rsp_types
	@echo ""; echo "━━━ test_dat_types ━━━"
	@$(RUN_ENV) ./build/test_dat_types
	@echo ""; echo "━━━ test_req_types ━━━"
	@$(RUN_ENV) ./build/test_req_types
	@echo ""; echo "━━━ test_snp_types ━━━"
	@$(RUN_ENV) ./build/test_snp_types
	@echo ""; echo "━━━ test_concurrent ━━━"
	@$(RUN_ENV) ./build/test_concurrent
	@echo ""; echo "━━━ test_channels ━━━"
	@$(RUN_ENV) ./build/test_channels
	@echo ""; echo "━━━ test_edge_cases ━━━"
	@$(RUN_ENV) ./build/test_edge_cases
	@echo ""; echo "━━━ test_ipc ━━━"
	@$(RUN_ENV) ./build/test_ipc
	@echo ""; echo "━━━ test_questa_top_ipc ━━━"
	@$(RUN_ENV) ./build/test_questa_top_ipc

clean:
	rm -rf build/

# ── Questa sccom build ────────────────────────────────────────────────────────
# Builds work/systemc.so + compiles SV wrapper for vsim IPC co-simulation.
# Requires Questa 2024 (QUESTA=) and SCC library built for Questa (LIBSOC=).
#
# NOTE: Questa's bundled ld (10.3.0) cannot link against modern glibc (RELR
# section type 0x13 in libm.so on Fedora 40+ / Ubuntu 24+).  This target
# replaces Questa's bundled ld with a shell wrapper that calls the system ld.
# That change is persistent (until Questa is reinstalled or ld.bak is restored).
# Run 'make questa_build_restore_ld' to undo.

QUESTA ?= $(HOME)/fedora/questa_24/questasim
QUESTA_LD := $(QUESTA)/gcc-10.3.0-linux_x86_64/libexec/gcc/x86_64-pc-linux-gnu/10.3.0/ld
# Questa's bundled g++ 10.3.0: produces GLIBCXX_3.4.26-compatible .so files
# that vsim can load (Questa's bundled libstdc++ tops out at GLIBCXX_3.4.28).
# System g++ produces GLIBCXX_3.4.30 symbols which Questa's libstdc++ cannot resolve.
QUESTA_CXX := $(QUESTA)/gcc-10.3.0-linux_x86_64/bin/g++
# System libstdc++.so.6 (resolved portably: Fedora uses /usr/lib64, Debian/Ubuntu
# uses /usr/lib/x86_64-linux-gnu).  vsim must LD_PRELOAD/stage this so the newer
# GLIBCXX symbols in systemc.so resolve against the system runtime.
LIBSTDCXX ?= $(realpath $(shell $(CC) -print-file-name=libstdc++.so.6))
QUESTA_BUILD := $(CURDIR)/questa_build
QUESTA_OBJDIR := $(QUESTA_BUILD)/work/_sc/linux_x86_64_gcc-10.3.0
# RTL DAT bus width in bits (128/256/512). MUST match the SV tb's
# CHIE_DATA_WIDTH_PARAM: it sets how many link-credits the bridge returns per
# 512-bit line (CHI_BEATS_PER_LINE = 512/CHI_DW). Default 256 = 2 beats/line.
CHI_DW ?= 256
SCCOM_FLAGS := -suppress sccom-94 -O2 -std=c++17 \
               -DCHI_DATA_BEAT_BITS=$(CHI_DW) \
               -I$(CURDIR)/questa \
               -I$(CURDIR)/ipc \
               -I$(LIBSOC)/include
SCCOM := PATH=$(QUESTA)/bin:$$PATH sccom $(SCCOM_FLAGS)
VLOG  := PATH=$(QUESTA)/bin:$$PATH vlog -work $(QUESTA_BUILD)/work -sv

questa_build: $(QUESTA_BUILD)/work/systemc.so $(QUESTA_BUILD)/work/questa_top_ipc_wrap
	@echo ""
	@echo "━━━ questa_build complete ━━━"
	@echo "  systemc.so : $(QUESTA_BUILD)/work/systemc.so"
	@echo "  To generate SV component stub:"
	@echo "    PATH=$(QUESTA)/bin:\$$PATH vgencomp -work $(QUESTA_BUILD)/work questa_top_ipc_vsim"

$(QUESTA_BUILD)/work:
	mkdir -p $(QUESTA_BUILD)
	cd $(QUESTA_BUILD) && PATH=$(QUESTA)/bin:$$PATH vlib work

# Patch Questa's bundled ld to use system ld (avoids RELR section errors)
questa_ld_patch:
	@if [ ! -f $(QUESTA_LD).bak ]; then \
	    echo "Patching Questa ld: $(QUESTA_LD)"; \
	    sudo cp $(QUESTA_LD) $(QUESTA_LD).bak; \
	    printf '#!/bin/sh\nexec /usr/bin/ld "$$@"\n' | sudo tee $(QUESTA_LD) > /dev/null; \
	    sudo chmod +x $(QUESTA_LD); \
	    echo "  Backup: $(QUESTA_LD).bak"; \
	else \
	    echo "Questa ld already patched ($(QUESTA_LD).bak exists)"; \
	fi

questa_build_restore_ld:
	@if [ -f $(QUESTA_LD).bak ]; then \
	    sudo cp $(QUESTA_LD).bak $(QUESTA_LD); \
	    sudo rm $(QUESTA_LD).bak; \
	    echo "Restored Questa ld from backup"; \
	else \
	    echo "No backup found at $(QUESTA_LD).bak"; \
	fi

$(QUESTA_BUILD)/work/_sc/linux_x86_64_gcc-10.3.0/chi_ipc.o: \
        ipc/chi_ipc.cc | $(QUESTA_BUILD)/work
	cd $(QUESTA_BUILD) && $(SCCOM) $(CURDIR)/ipc/chi_ipc.cc

$(QUESTA_BUILD)/work/_sc/linux_x86_64_gcc-10.3.0/questa_top_ipc.o: \
        questa/questa_top_ipc.cc questa/questa_top_ipc.hh \
        questa/chi_flit_cfg.hh questa/chi_flit_pack.hh ipc/chi_ipc.hh \
        | $(QUESTA_BUILD)/work
	cd $(QUESTA_BUILD) && $(SCCOM) $(CURDIR)/questa/questa_top_ipc.cc

$(QUESTA_BUILD)/work/_sc/linux_x86_64_gcc-10.3.0/questa_top_ipc_vsim.o: \
        questa/questa_top_ipc_vsim.cc questa/questa_top_ipc_vsim.hh \
        | $(QUESTA_BUILD)/work
	cd $(QUESTA_BUILD) && $(SCCOM) $(CURDIR)/questa/questa_top_ipc_vsim.cc

$(QUESTA_BUILD)/work/_sc/linux_x86_64_gcc-10.3.0/questa_cosim_top_ipc.o: \
        questa/questa_cosim_top_ipc.cc questa/questa_top_ipc_vsim.hh \
        | $(QUESTA_BUILD)/work
	cd $(QUESTA_BUILD) && $(SCCOM) $(CURDIR)/questa/questa_cosim_top_ipc.cc

$(QUESTA_BUILD)/work/_sc/linux_x86_64_gcc-10.3.0/questa_sc_main.o: \
        questa/questa_sc_main.cc \
        | $(QUESTA_BUILD)/work
	cd $(QUESTA_BUILD) && $(SCCOM) $(CURDIR)/questa/questa_sc_main.cc

# systemc.so (standalone vsim, no SV wrapper): uses questa_cosim_top_ipc which
# has a static questa_top_ipc_vsim instance that vsim connects via co-sim binding.
# Must link with QUESTA_CXX (gcc-10.3.0) so the .so uses GLIBCXX ≤ 3.4.28,
# which is what Questa's bundled libstdc++.so.6 provides.  System g++ (14.x)
# produces GLIBCXX_3.4.30 symbols that Questa's runtime cannot resolve.
$(QUESTA_BUILD)/work/systemc.so: \
        $(QUESTA_OBJDIR)/chi_ipc.o \
        $(QUESTA_OBJDIR)/questa_top_ipc.o \
        $(QUESTA_OBJDIR)/questa_top_ipc_vsim.o \
        $(QUESTA_OBJDIR)/questa_cosim_top_ipc.o \
        | questa_ld_patch
	$(QUESTA_CXX) -std=c++17 -shared -fPIC -o $@ $^ \
	    $(QUESTA)/linux_x86_64/libsystemc_cpp17.a -lrt

# systemc_sv.so (SV-wrapper vsim, questa_top_ipc_wrap.sv as top):
# uses sccom -link (NOT a manual g++ -shared) so that vopt can verify the
# SystemC version marker embedded by sccom.  Manual g++ -shared produces a
# .so that fails vopt-10515 ("No 64-bit systemc.so found for SystemC version
# 2.3.2 compiled with gcc-10.3.0").
#
# sccom -link links ALL .o files currently in the work library.  For the SV
# variant we need only: chi_ipc + questa_top_ipc + questa_top_ipc_vsim +
# questa_sc_main.  questa_cosim_top_ipc.o (standalone sc_main) conflicts, so
# it is removed before linking and restored for the standalone systemc.so build.
$(QUESTA_BUILD)/work/systemc_sv.so: \
        $(QUESTA_OBJDIR)/chi_ipc.o \
        $(QUESTA_OBJDIR)/questa_top_ipc.o \
        $(QUESTA_OBJDIR)/questa_top_ipc_vsim.o \
        $(QUESTA_OBJDIR)/questa_sc_main.o \
        | questa_ld_patch
	# Remove standalone sc_main .o so only questa_sc_main (minimal) is linked
	rm -f $(QUESTA_OBJDIR)/questa_cosim_top_ipc.o
	rm -f $(QUESTA_OBJDIR)/questa_cosim_top_ipc.dbs
	cd $(QUESTA_BUILD) && PATH=$(QUESTA)/bin:$$PATH sccom -suppress sccom-94 -link
	cp $(QUESTA_OBJDIR)/systemc.so $@
	# Copy system libstdc++ alongside systemc.so so the dynamic linker finds
	# it before Questa's bundled copy (which lacks GLIBCXX_3.4.29).
	cp $(LIBSTDCXX) $(QUESTA_OBJDIR)/libstdc++.so.6

$(QUESTA_BUILD)/work/questa_top_ipc_wrap: questa/questa_top_ipc_wrap.sv \
        | $(QUESTA_BUILD)/work/systemc.so
	$(VLOG) $(CURDIR)/questa/questa_top_ipc_wrap.sv
	touch $@

$(QUESTA_BUILD)/work/tb_ipc_vsim: questa/tb_ipc_vsim.sv \
        | $(QUESTA_BUILD)/work/questa_top_ipc_wrap
	$(VLOG) $(CURDIR)/questa/tb_ipc_vsim.sv
	touch $@

$(QUESTA_BUILD)/work/tb_gem5_cosim: questa/tb_gem5_cosim.sv \
        | $(QUESTA_BUILD)/work/questa_top_ipc_wrap
	$(VLOG) $(CURDIR)/questa/tb_gem5_cosim.sv
	touch $@

# Build the SV testbench variant (systemc_sv.so + SV files, standalone with shm_create)
questa_build_sv: $(QUESTA_BUILD)/work/systemc_sv.so $(QUESTA_BUILD)/work/tb_ipc_vsim
	@echo ""
	@echo "━━━ questa_build_sv complete ━━━"
	@echo "  systemc_sv.so : $(QUESTA_BUILD)/work/systemc_sv.so"
	@echo "  To run the SV testbench (standalone, no gem5):"
	@echo "    questa_build/shm_create &"
	@echo "    PATH=$(QUESTA)/bin:\$$PATH vsim -c \\"
	@echo "        -lib questa_build/work questa_top_ipc_wrap tb_ipc_vsim \\"
	@echo "        -do 'run -all; quit -f'"

# Build the gem5 co-simulation testbench (requires gem5 --vip running in parallel)
questa_build_cosim: $(QUESTA_BUILD)/work/systemc_sv.so $(QUESTA_BUILD)/work/tb_gem5_cosim
	@echo ""
	@echo "━━━ questa_build_cosim complete ━━━"
	@echo "  To run: start gem5 first, then run questa:"
	@echo ""
	@echo "  Terminal 1 (gem5):"
	@echo "    cd gem5/"
	@echo "    build/X86_CHI/gem5.opt ../config/chi/chi_hnf_only.py --max-ticks 500000000000000"
	@echo ""
	@echo "  Terminal 2 (questa):"
	@echo "    cd chi_vip/"
	@echo "    LM_LICENSE_FILE=1718@localhost \\"
	@echo "    LD_PRELOAD=$(LIBSTDCXX) \\"
	@echo "    PATH=$(QUESTA)/bin:\$$PATH \\"
	@echo "    vsim -c -lib questa_build/work questa_top_ipc_wrap tb_gem5_cosim \\"
	@echo "        -do 'run -all; quit -f'"

# ── cva6-RNF SC rebuild ───────────────────────────────────────────────────────
# Rebuilds the SC co-sim library in the cva6-RNF work directory.
# Must be re-run whenever chi_vip SC sources change or work/ is cleaned.
# RNF_WORK must point to the cva6-RNF Questa work library directory.
RNF_WORK ?= $(abspath $(CURDIR)/../../work/chi-rnf/cva6-RNF/work)
RNF_OBJDIR = $(RNF_WORK)/_sc/linux_x86_64_gcc-10.3.0
RNF_SCCOM  = PATH=$(QUESTA)/bin:$$PATH sccom -suppress sccom-94 -O2 -std=c++17 \
             -DCHI_DATA_BEAT_BITS=$(CHI_DW) \
             -work $(RNF_WORK) \
             -I$(CURDIR)/questa -I$(CURDIR)/ipc -I$(LIBSOC)/include

rnf_build_sc:
	# Compile SC sources into cva6-RNF work library
	$(RNF_SCCOM) $(CURDIR)/ipc/chi_ipc.cc
	$(RNF_SCCOM) $(CURDIR)/questa/questa_top_ipc.cc
	$(RNF_SCCOM) $(CURDIR)/questa/questa_top_ipc_vsim.cc
	$(RNF_SCCOM) $(CURDIR)/questa/questa_sc_main.cc
	# Remove any conflicting standalone-sc_main .o files
	rm -f $(RNF_OBJDIR)/questa_cosim_top_ipc.o \
	      $(RNF_OBJDIR)/questa_cosim_top_ipc.dbs \
	      $(RNF_OBJDIR)/questa_cosim_top_ipc.o.version \
	      $(RNF_OBJDIR)/questa_cosim_top.o \
	      $(RNF_OBJDIR)/questa_cosim_top.dbs \
	      $(RNF_OBJDIR)/questa_cosim_top.o.version \
	      $(RNF_OBJDIR)/questa_top.o \
	      $(RNF_OBJDIR)/questa_top.dbs \
	      $(RNF_OBJDIR)/questa_top.o.version
	# Link with sccom to embed SystemC version marker (required by vopt-10515)
	cd $(dir $(RNF_WORK)) && PATH=$(QUESTA)/bin:$$PATH sccom -suppress sccom-94 -work $(RNF_WORK) -link
	# Deploy to work/systemc.so and stage system libstdc++
	cp $(RNF_OBJDIR)/systemc.so $(RNF_WORK)/systemc.so
	cp $(LIBSTDCXX) $(RNF_OBJDIR)/libstdc++.so.6
	@echo ""
	@echo "━━━ rnf_build_sc complete ━━━"
	@echo "  $(RNF_WORK)/systemc.so updated"
	@echo "  Run vsim with: LD_PRELOAD=$(LIBSTDCXX)"

.PHONY: all run clean cosim-help questa_build questa_build_sv questa_build_cosim questa_ld_patch questa_build_restore_ld rnf_build_sc

# ── Co-simulation build help ──────────────────────────────────────────────────
cosim-help:
	@echo ""
	@echo "CHI VIP co-simulation build targets:"
	@echo "  build/libchi_ipc.so         — POSIX shm IPC layer (gem5 + questa sides)"
	@echo "  build/libchi_questa_ipc.so  — questa_top_ipc: Questa co-sim bridge"
	@echo ""
	@echo "gem5 build (from gem5/ directory):"
	@echo "  scons build/X86/gem5.opt PROTOCOL=CHI \\"
	@echo "        CHI_VIP_IPC_PATH=$(CURDIR)/ipc"
	@echo ""
	@echo "questa_top_ipc usage in questa_cosim_top_ipc.cc:"
	@echo "  questa_top_ipc qt(\"qt\", \"/chi_vip_0\", 1000 /*ps*/);"
	@echo "  // bind qt ports to DUT, then sc_start()"
