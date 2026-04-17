// questa_sc_main.cc — minimal sc_main for vsim SV-wrapper co-simulation.
//
// When questa_top_ipc_wrap.sv instantiates questa_top_ipc_vsim from SV,
// vsim creates the SystemC module instance automatically via SC_MODULE_EXPORT
// and binds its ports to the SV signals.  sc_main must exist in the .so but
// must NOT create any SC module instances — doing so produces a second
// unbound instance that crashes at t=0 ("port is not bound" vsim-6513).
//
// questa_cosim_top_ipc.cc is the standalone variant (no SV top): it creates
// a static questa_top_ipc_vsim and expects vsim to connect it via its
// co-sim port-binding mechanism without an SV wrapper.

#include <systemc>

int sc_main(int /*argc*/, char** /*argv*/)
{
    return 0;
}
