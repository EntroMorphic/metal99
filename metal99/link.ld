/* Zero-dependency ESP32-S3 image. Loaded by the mask-ROM bootloader from
 * flash offset 0x0 straight into internal SRAM. No 2nd-stage bootloader,
 * no partition table, no flash XIP, no MMU/cache setup.
 *
 * SRAM1 is dual-mapped: IRAM 0x40378000 == DRAM 0x3FC88000 (offset 0x6F0000).
 * We keep well below 0x3FCE9700, which the ROM uses for its own stack. */
ENTRY(_start)

MEMORY
{
  iram (RWX) : ORIGIN = 0x40378000, LENGTH = 0x20000   /* 128K code   */
  dram (RW)  : ORIGIN = 0x3FCA8000, LENGTH = 0x30000   /* 192K data   */
}

SECTIONS
{
  .text : ALIGN(4) {
    /* Xtensa L32R reads literals at a NEGATIVE pc offset: every .literal
     * must be emitted before the .text that references it. */
    *(.entry.literal)
    *(.entry.text)
    *(.literal .text .literal.* .text.*)
  } > iram

  .rodata : ALIGN(4) { *(.rodata .rodata.*) } > dram
  .data   : ALIGN(4) { *(.data .data.*) }     > dram

  .bss : ALIGN(4) {
    _bss_start = ABSOLUTE(.);
    *(.bss .bss.* COMMON)
    _bss_end = ABSOLUTE(.);
  } > dram

  _stack_top = ORIGIN(dram) + LENGTH(dram) - 16;
}
