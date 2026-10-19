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

// test_opcode_maps.cc
// Verifies that every CHIRequestType that has a defined SCC mapping round-trips
// through gem5req_to_scc() → scc_to_gem5req() back to the original value,
// and that every CHIRequestType snoop maps through gem5req_to_scc_snp().

#include <systemc>
#include <cstdio>
#include <cstring>

#include "mem/ruby/system/RubySystem.hh"
#include "mem/ruby/network/Network.hh"
#include "mem/ruby/common/MachineID.hh"
#include "chi_vip_controller.hh"

using gem5::ruby::CHI_VIP_Controller;
using namespace gem5::ruby::CHI;
using chi::req_optype_e;
using chi::snp_optype_e;

// ── helpers ──────────────────────────────────────────────────────────────────

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { printf("  PASS: %s\n", msg); g_pass++; } \
        else      { printf("  FAIL: %s\n", msg); g_fail++; } \
    } while(0)

// ── REQ round-trip pairs that must be stable ─────────────────────────────────
// (gem5 type, scc opcode) — if the reverse mapping is unique
struct ReqPair { CHIRequestType gem5; req_optype_e scc; };

static const ReqPair req_pairs[] = {
    { CHIRequestType_ReadShared,         req_optype_e::ReadShared         },
    { CHIRequestType_ReadNotSharedDirty, req_optype_e::ReadNotSharedDirty },
    { CHIRequestType_ReadUnique,         req_optype_e::ReadUnique         },
    { CHIRequestType_ReadOnce,           req_optype_e::ReadOnce           },
    { CHIRequestType_CleanUnique,        req_optype_e::CleanUnique        },
    { CHIRequestType_MakeReadUnique,     req_optype_e::MakeReadUnique     },
    { CHIRequestType_Evict,              req_optype_e::Evict              },
    { CHIRequestType_WriteBackFull,      req_optype_e::WriteBackFull      },
    { CHIRequestType_WriteCleanFull,     req_optype_e::WriteCleanFull     },
    { CHIRequestType_WriteEvictFull,     req_optype_e::WriteEvictFull     },
    { CHIRequestType_WriteUniquePtl,     req_optype_e::WriteUniquePtl     },
    { CHIRequestType_WriteUniqueFull,    req_optype_e::WriteUniqueFull    },
    { CHIRequestType_WriteNoSnpPtl,      req_optype_e::WriteNoSnpPtl      },
    { CHIRequestType_WriteNoSnp,         req_optype_e::WriteNoSnpFull     },
    { CHIRequestType_ReadNoSnp,          req_optype_e::ReadNoSnp          },
    { CHIRequestType_StashOnceShared,    req_optype_e::StashOnceShared    },
    { CHIRequestType_StashOnceUnique,    req_optype_e::StashOnceUnique    },
    { CHIRequestType_AtomicReturn,       req_optype_e::AtomicSwap         },
    { CHIRequestType_AtomicNoReturn,     req_optype_e::AtomicCompare      },
    { CHIRequestType_WriteUniqueZero,    req_optype_e::WriteUniqueZero    },
};

// ── SNP one-way pairs (gem5 → SCC) ───────────────────────────────────────────
struct SnpPair { CHIRequestType gem5; snp_optype_e scc; };

static const SnpPair snp_pairs[] = {
    { CHIRequestType_SnpShared,            snp_optype_e::SnpShared            },
    { CHIRequestType_SnpUnique,            snp_optype_e::SnpUnique            },
    { CHIRequestType_SnpOnce,              snp_optype_e::SnpOnce              },
    { CHIRequestType_SnpCleanInvalid,      snp_optype_e::SnpCleanInvalid      },
    { CHIRequestType_SnpSharedFwd,         snp_optype_e::SnpSharedFwd         },
    { CHIRequestType_SnpNotSharedDirtyFwd, snp_optype_e::SnpNotSharedDirtyFwd },
    { CHIRequestType_SnpUniqueFwd,         snp_optype_e::SnpUniqueFwd         },
    { CHIRequestType_SnpOnceFwd,           snp_optype_e::SnpOnceFwd           },
    { CHIRequestType_SnpDvmOpSync_P1,      snp_optype_e::SnpDVMOp             },
    { CHIRequestType_SnpDvmOpNonSync_P1,   snp_optype_e::SnpDVMOp             },
};

int sc_main(int, char**) {
    sc_core::sc_set_time_resolution(1, sc_core::SC_PS);

    printf("\n=== test_opcode_maps ===\n");

    // ── REQ forward mapping ───────────────────────────────────────────────────
    printf("\n[REQ forward: gem5 → SCC]\n");
    for (auto& p : req_pairs) {
        req_optype_e got = CHI_VIP_Controller::gem5req_to_scc(p.gem5);
        char msg[128];
        snprintf(msg, sizeof(msg), "gem5req_to_scc(%d) == expected SCC opcode %d",
                 (int)p.gem5, (int)p.scc);
        CHECK(got == p.scc, msg);
    }

    // ── REQ reverse mapping ───────────────────────────────────────────────────
    printf("\n[REQ reverse: SCC → gem5 (round-trip)]\n");
    for (auto& p : req_pairs) {
        CHIRequestType got = CHI_VIP_Controller::scc_to_gem5req(p.scc);
        char msg[128];
        snprintf(msg, sizeof(msg), "scc_to_gem5req(SCC %d) round-trips to gem5 %d",
                 (int)p.scc, (int)p.gem5);
        CHECK(got == p.gem5, msg);
    }

    // ── SNP forward mapping ───────────────────────────────────────────────────
    printf("\n[SNP forward: gem5 → SCC]\n");
    for (auto& p : snp_pairs) {
        snp_optype_e got = CHI_VIP_Controller::gem5req_to_scc_snp(p.gem5);
        char msg[128];
        snprintf(msg, sizeof(msg), "gem5req_to_scc_snp(%d) == expected SNP opcode %d",
                 (int)p.gem5, (int)p.scc);
        CHECK(got == p.scc, msg);
    }

    printf("\n=== RESULTS: %d passed, %d failed ===\n\n", g_pass, g_fail);
    return g_fail != 0 ? 1 : 0;
}
