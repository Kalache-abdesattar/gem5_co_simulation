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

