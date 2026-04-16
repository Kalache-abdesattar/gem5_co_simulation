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
