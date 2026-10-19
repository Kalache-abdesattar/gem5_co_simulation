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

#pragma once
// questa_top_ipc_vsim.hh — flat (non-namespaced) SC_MODULE wrapper for vsim
//
// Questa's sccom/vgencomp cannot detect SC_MODULEs declared inside C++
// namespaces.  This header exposes an SC_MODULE at global scope that contains
// an embedded instance of gem5::ruby::questa_top_ipc and connects its ports
// via sc_signal<>.  vgencomp sees questa_top_ipc_vsim and generates the SV
// component stub.
//
// The SV wrapper should instantiate questa_top_ipc_vsim (or its vgencomp-
// generated stub gem5__ruby__questa_top_ipc_vsim) and connect to the DUT.
//
// Configuration: pass VIP_SHM_NAME and VIP_CYCLE_PS at compile time.

#include <systemc>
#include "questa_top_ipc.hh"

#ifndef VIP_SHM_NAME
#  define VIP_SHM_NAME "/chi_vip_0"
#endif
#ifndef VIP_CYCLE_PS
#  define VIP_CYCLE_PS 1000
#endif

using namespace sc_core;
using namespace sc_dt;
using ReqFlit = sc_bv<chi_flit::CHI_REQ_FLIT_W>;
using RspFlit = sc_bv<chi_flit::CHI_RSP_FLIT_W>;
using DatFlit = sc_lv<chi_flit::CHI_DAT_FLIT_W>;  // must match questa_top_ipc.hh
using SnpFlit = sc_bv<chi_flit::CHI_SNP_FLIT_W>;

// ─────────────────────────────────────────────────────────────────────────────
// questa_top_ipc_vsim — vgencomp-visible flat wrapper
//
// This module has exactly the same 22 ports as questa_top_ipc but lives at
// global scope so Questa's module registry finds it.  Ports are connected to
// the internal impl_ via sc_signal<>.
// ─────────────────────────────────────────────────────────────────────────────
SC_MODULE(questa_top_ipc_vsim)
{
    // ── Ports (same names/directions as questa_top_ipc) ───────────────────────
    sc_in<bool>    clk  {"clk"};
    sc_in<bool>    rstn {"rstn"};

    sc_in<bool>     txreq_flitv    {"txreq_flitv"};
    sc_in<bool>     txreq_flitpend {"txreq_flitpend"};
    sc_in<ReqFlit>  txreq_flit     {"txreq_flit"};
    sc_out<bool>    txreq_lcrdv    {"txreq_lcrdv"};

    sc_in<bool>     txrsp_flitv    {"txrsp_flitv"};
    sc_in<bool>     txrsp_flitpend {"txrsp_flitpend"};
    sc_in<RspFlit>  txrsp_flit     {"txrsp_flit"};
    sc_out<bool>    txrsp_lcrdv    {"txrsp_lcrdv"};

    sc_in<bool>     txdat_flitv    {"txdat_flitv"};
    sc_in<bool>     txdat_flitpend {"txdat_flitpend"};
    sc_in<DatFlit>  txdat_flit     {"txdat_flit"};
    sc_out<bool>    txdat_lcrdv    {"txdat_lcrdv"};

    sc_out<bool>    rxrsp_flitv    {"rxrsp_flitv"};
    sc_out<bool>    rxrsp_flitpend {"rxrsp_flitpend"};
    sc_out<RspFlit> rxrsp_flit     {"rxrsp_flit"};
    sc_in<bool>     rxrsp_lcrdv    {"rxrsp_lcrdv"};

    sc_out<bool>    rxdat_flitv    {"rxdat_flitv"};
    sc_out<bool>    rxdat_flitpend {"rxdat_flitpend"};
    sc_out<DatFlit> rxdat_flit     {"rxdat_flit"};
    sc_in<bool>     rxdat_lcrdv    {"rxdat_lcrdv"};

    sc_out<bool>    rxsnp_flitv    {"rxsnp_flitv"};
    sc_out<bool>    rxsnp_flitpend {"rxsnp_flitpend"};
    sc_out<SnpFlit> rxsnp_flit     {"rxsnp_flit"};
    sc_in<bool>     rxsnp_lcrdv    {"rxsnp_lcrdv"};

    SC_HAS_PROCESS(questa_top_ipc_vsim);

    explicit questa_top_ipc_vsim(sc_module_name n)
        : sc_module(n)
        , impl_("impl", VIP_SHM_NAME, (uint64_t)VIP_CYCLE_PS)
    {
        // Bind impl_ ports to our ports directly (SC port chaining)
        impl_.clk            (clk);
        impl_.rstn           (rstn);
        impl_.txreq_flitv    (txreq_flitv);
        impl_.txreq_flitpend (txreq_flitpend);
        impl_.txreq_flit     (txreq_flit);
        impl_.txreq_lcrdv    (txreq_lcrdv);
        impl_.txrsp_flitv    (txrsp_flitv);
        impl_.txrsp_flitpend (txrsp_flitpend);
        impl_.txrsp_flit     (txrsp_flit);
        impl_.txrsp_lcrdv    (txrsp_lcrdv);
        impl_.txdat_flitv    (txdat_flitv);
        impl_.txdat_flitpend (txdat_flitpend);
        impl_.txdat_flit     (txdat_flit);
        impl_.txdat_lcrdv    (txdat_lcrdv);
        impl_.rxrsp_flitv    (rxrsp_flitv);
        impl_.rxrsp_flitpend (rxrsp_flitpend);
        impl_.rxrsp_flit     (rxrsp_flit);
        impl_.rxrsp_lcrdv    (rxrsp_lcrdv);
        impl_.rxdat_flitv    (rxdat_flitv);
        impl_.rxdat_flitpend (rxdat_flitpend);
        impl_.rxdat_flit     (rxdat_flit);
        impl_.rxdat_lcrdv    (rxdat_lcrdv);
        impl_.rxsnp_flitv    (rxsnp_flitv);
        impl_.rxsnp_flitpend (rxsnp_flitpend);
        impl_.rxsnp_flit     (rxsnp_flit);
        impl_.rxsnp_lcrdv    (rxsnp_lcrdv);
    }

  private:
    gem5::ruby::questa_top_ipc impl_;
};
