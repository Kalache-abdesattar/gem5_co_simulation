OUTPUT_ARCH( "riscv" )
ENTRY(_start)

SECTIONS
{
  . = 0x80000000;

  .text.init : { *(.text.init) }
  .text      : { *(.text) }
  .rodata    : { *(.rodata .rodata.*) }

  .data  : { *(.data) }
  .sdata : {
    __global_pointer$ = . + 0x800;
    *(.srodata*) *(.sdata .sdata.* .gnu.linkonce.s.*)
  }

  . = ALIGN(16);
  __bss_start = .;
  .bss : {
    . = ALIGN(16);
    *(.sbss .sbss.* .gnu.linkonce.sb.*) *(.scommon)
    *(.bss .bss.*) *(COMMON)
  }
  __bss_end = .;

  . = ALIGN(16);
  .tdata : { _tls_data = .; *(.tdata.begin) *(.tdata) *(.tdata.end) }
  . = ALIGN(16);
  .tbss  : { *(.tbss) *(.tbss.end) }

  . = ALIGN(16);
  _end = .;

  /* tohost/fromhost at a fixed address so gem5 can poll for test completion */
  . = 0x80040000;
  .tohost : { *(.tohost) }
}
