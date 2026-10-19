// Copyright (c) 2025-2026 Abdesattar Kalache <abdesattar.kalache@tuni.fi>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

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
