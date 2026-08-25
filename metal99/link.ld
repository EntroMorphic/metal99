/* Zero-dependency ESP32-S3 image. Loaded by the mask-ROM bootloader from
 * flash offset 0x0 straight into internal SRAM. No 2nd-stage bootloader,
 * no partition table, no flash XIP, no MMU/cache setup.
 *
 * SRAM1 is dual-mapped: IRAM 0x40378000 == DRAM 0x3FC88000 (offset 0x6F0000).
 * We keep well below 0x3FCE9700, which the ROM uses for its own stack. */
ENTRY(_start)

/* THESE ARE CHOICES, NOT LIMITS.
 *
 * The chip has 480 KB of DRAM-addressable SRAM. We claim 192 KB of it and use
 * about 96; IRAM claims 128 KB for under 11 KB of code. Both are deliberately
 * generous and deliberately unchanged - the map is verified working, and there
 * is no reason to move a working memory map.
 *
 * Stated because "322 KB will not fit in 192 KB of DRAM" was repeated for a
 * while as the reason this project has no framebuffer. It is false: right-
 * sizing IRAM would leave ~373 KB below the ROM stack floor, and a 322 KB
 * framebuffer fits with room over. The real reason is that the panel is
 * wire-bound and storing pixels does not send fewer of them. See
 * docs/DESIGN.md 5.1 for the arithmetic. */
MEMORY
{
  iram (RWX) : ORIGIN = 0x40378000, LENGTH = 0x20000   /* 128K claimed, ~11K used  */
  dram (RW)  : ORIGIN = 0x3FCA8000, LENGTH = 0x30000   /* 192K claimed, ~96K used  */
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

  /* 16-byte aligned, and padded to a whole number of 128-bit vectors, so
   * vec_zero() covers it exactly with no scalar tail. */
  .bss : ALIGN(16) {
    _bss_start = ABSOLUTE(.);
    *(.bss .bss.* COMMON)
    . = ALIGN(16);
    _bss_end = ABSOLUTE(.);
  } > dram

  /* Provided, but nothing references it: _start runs on the stack the mask ROM
   * hands over, which lives just under 0x3FCE9700 - above this region, growing
   * down, with ~70 KB of clearance before it could reach our data. Kept as the
   * anchor for the day this image wants its own stack; check `nm` for a
   * reference before assuming it is live. */
  _stack_top = ORIGIN(dram) + LENGTH(dram) - 16;
}
