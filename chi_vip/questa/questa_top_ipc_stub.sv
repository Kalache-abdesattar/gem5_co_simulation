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

component questa_top_ipc_vsim 

    port(

        clk : in std_logic;

        rstn : in std_logic;

        txreq_flitv : in std_logic;

        txreq_flitpend : in std_logic;

        txreq_flit : in std_logic_vector(112 downto 0);

        txreq_lcrdv : out std_logic;

        txrsp_flitv : in std_logic;

        txrsp_flitpend : in std_logic;

        txrsp_flit : in std_logic_vector(55 downto 0);

        txrsp_lcrdv : out std_logic;

        txdat_flitv : in std_logic;

        txdat_flitpend : in std_logic;

        txdat_flit : in std_logic_vector(636 downto 0);

        txdat_lcrdv : out std_logic;

        rxrsp_flitv : out std_logic;

        rxrsp_flitpend : out std_logic;

        rxrsp_flit : out std_logic_vector(55 downto 0);

        rxrsp_lcrdv : in std_logic;

        rxdat_flitv : out std_logic;

        rxdat_flitpend : out std_logic;

        rxdat_flit : out std_logic_vector(636 downto 0);

        rxdat_lcrdv : in std_logic;

        rxsnp_flitv : out std_logic;

        rxsnp_flitpend : out std_logic;

        rxsnp_flit : out std_logic_vector(84 downto 0);

        rxsnp_lcrdv : in std_logic

    );

end component;

