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

#include <systemc>
#include "mem/ruby/system/RubySystem.hh"
#include "mem/ruby/network/Network.hh"
#include "mem/ruby/common/MachineID.hh"
#include "chi_vip_controller.hh"

using gem5::ruby::CHI_VIP_Controller;

int sc_main(int, char**) {
    sc_core::sc_set_time_resolution(1, sc_core::SC_PS);

    gem5::ruby::RubySystem rs;
    rs.m_cache_line_size = 64;

    Network net;

    MachineID rnf_id = { MachineType_Cache,     0 };
    MachineID hnf_id = { MachineType_Directory, 0 };

    // sc_module_name is passed as first arg so sockets see the module hierarchy
    CHI_VIP_Controller ctrl("rnf0", rnf_id, &rs, &net);
    ctrl.downstream_ = hnf_id;

    ctrl.init();

    sc_core::sc_start(1, sc_core::SC_NS);

    printf("PASS: init() and first tick completed\n");
    return 0;
}
