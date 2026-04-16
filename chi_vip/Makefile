GEM5_SRC  := $(CURDIR)/gem5_src
SHIM      := $(CURDIR)/chi_shim
LIBSOC    := /home/dmk/fedora/scc_install
SC_ROOT   := /home/dmk/fedora/systemc

CXX       := g++
CXXFLAGS  := -std=c++17 -O2 -fPIC -Wall -Wno-unused-variable \
             -I$(SHIM)              \
             -I$(GEM5_SRC)          \
             -I$(LIBSOC)/include    \
             -I$(SC_ROOT)/src       \
             -Isc                   \
             -Iquesta

SC_LIBS   := -L$(SC_ROOT)/build/src -lsystemc \
             -Wl,-rpath,$(SC_ROOT)/build/src

OBJS      := build/CHIGenericController.o \
             build/utils.o                \
             build/chi_vip_controller.o

QUESTA_OBJS := build/questa_top.o

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
             build/test_edge_cases

all: build/libchi_vip.so build/libchi_questa.so $(TESTS)

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

clean:
	rm -rf build/

.PHONY: all run clean
