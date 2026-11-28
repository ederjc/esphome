/* QEMU mps2-an385 Cortex-M3 memory map, independent of the PSoC 6 board. */
ENTRY(reset)
SECTIONS {
  . = 0x0;
  .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) }
  . = 0x20000000;
  .data : { *(.data*) }
  .bss : { *(.bss*) *(COMMON) }
}
