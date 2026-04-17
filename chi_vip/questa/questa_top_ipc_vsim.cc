// questa_top_ipc_vsim.cc — compilation unit for the vgencomp-visible wrapper
//
// questa_top_ipc_vsim is defined in questa_top_ipc_vsim.hh.
// SC_MODULE_EXPORT registers the module with Questa's vsim kernel so that:
//   - vgencomp can find it and generate the SV component stub
//   - vsim can elaborate it as a top-level co-sim design unit
//
// This is a Questa-specific macro defined in <systemc/mti_integration.h>.
// It is NOT needed for standalone builds (sccom's module db doesn't use it).

#include "questa_top_ipc_vsim.hh"

SC_MODULE_EXPORT(questa_top_ipc_vsim);
