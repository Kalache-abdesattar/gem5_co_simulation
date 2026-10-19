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
