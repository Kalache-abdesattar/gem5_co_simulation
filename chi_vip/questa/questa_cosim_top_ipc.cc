// questa_cosim_top_ipc.cc — sc_main for Questa vsim IPC co-simulation
//
// This file is compiled with sccom and linked with sccom -link to produce
// the systemc.so loaded by vsim.  It instantiates questa_top_ipc_vsim (a
// flat non-namespaced wrapper detected by Questa's sccom/vgencomp) and
// returns from sc_main immediately — vsim's SystemC kernel takes over.
// Port binding is handled automatically by vgencomp-generated stubs.
//
// IMPORTANT: Questa's sccom EDG parser does not register SC_MODULEs that
// are declared inside C++ namespaces.  questa_top_ipc_vsim (defined in
// questa_top_ipc_vsim.hh) lives at global scope and wraps the namespaced
// questa_top_ipc via SC port chaining.  vgencomp sees questa_top_ipc_vsim
// and generates the SV stub with the correct 22-port list.
//
// Configuration (override at sccom time with -D):
//   VIP_SHM_NAME  — POSIX shm segment name, default "/chi_vip_0"
//                   Must match the shm_name in gem5's VIPController param.
//   VIP_CYCLE_PS  — clock period in picoseconds, default 1000 (= 1 ns)
//
// Build (from chi_vip/ directory):
//   QUESTA=/path/to/questa_24/questasim
//   LIBSOC=/path/to/scc_install_questa
//   CHIDIR=$(pwd)
//
//   vlib work
//
//   sccom -suppress sccom-94 -O2 \
//       -I $CHIDIR/questa \
//       -I $CHIDIR/ipc \
//       -I $LIBSOC/include \
//       $CHIDIR/ipc/chi_ipc.cc \
//       $CHIDIR/questa/questa_top_ipc.cc \
//       $CHIDIR/questa/questa_cosim_top_ipc.cc
//
//   # Link with system g++ (Questa's bundled ld 10.3.0 cannot handle
//   # modern glibc RELR sections on Fedora 40+ / Ubuntu 24+):
//   OBJDIR=work/_sc/linux_x86_64_gcc-10.3.0
//   g++ -std=c++17 -shared -fPIC -o work/systemc.so \
//       $OBJDIR/chi_ipc.o $OBJDIR/questa_top_ipc.o \
//       $OBJDIR/questa_cosim_top_ipc.o \
//       $QUESTA/linux_x86_64/libsystemc_cpp17.a -lrt
//
//   # Generate Questa SV component stub
//   vgencomp -work work questa_top_ipc_vsim
//
//   # Compile questa_top_ipc_wrap_ipc.sv (IPC variant of questa_top_wrap.sv):
//   vlog -work work -sv $CHIDIR/questa/questa_top_ipc_wrap.sv
//
//   # Start gem5 first, then:
//   vsim -sc top [+other_vsim_args]

#include <systemc>
#include <cstdio>
#include "questa_top_ipc_vsim.hh"

// questa_top_ipc_vsim must outlive sc_main's return — Questa owns the SC kernel.
static questa_top_ipc_vsim g_qt("qt");

int sc_main(int /*argc*/, char** /*argv*/)
{
    printf("[questa_cosim_top_ipc] shm=\"%s\" cycle=%u ps\n",
           VIP_SHM_NAME, (unsigned)VIP_CYCLE_PS);
    printf("[questa_cosim_top_ipc] SC elaboration done, vsim takes over.\n");
    // Port binding is handled by vgencomp-generated stub for questa_top_ipc_vsim.
    // questa_top_ipc::start_of_simulation() will spin until gem5 creates shm.
    return 0;
}
