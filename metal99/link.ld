/* Zero-dependency ESP32-S3 image. Loaded by the mask-ROM bootloader from
 * flash offset 0x0 straight into internal SRAM. No 2nd-stage bootloader,
 * no partition table, no flash XIP, no MMU/cache setup.
 *
 * SRAM1 is dual-mapped: IRAM 0x40378000 == DRAM 0x3FC88000 (offset 0x6F0000).
 * We keep well below 0x3FCE9700, which the ROM uses for its own stack. */
ENTRY(_start)

/* RIGHT-SIZED, 2026-08-26, because something finally needed the room.
 *
 * IRAM claimed 128 KB to hold 14 KB of code, and because SRAM1 is dual-mapped
 * that claim came straight out of DRAM. This note used to say the map was
 * "deliberately unchanged - there is no reason to move a working memory map",
 * which was true right up until tile-granular elision at 8-row tiles wanted
 * 86 KB of band and hashes and the linker refused it by 13,984 bytes.
 *
 * IRAM is 32 KB now: 2.2x headroom over .text, and the whole of the rest goes
 * to DRAM. 320 KB claimed, ~205 KB used, and the region still stops 38 KB
 * short of 0x3FCE9700 where the ROM stack lives - we run on that stack, so
 * that clearance is not optional.
 *
 * The old note also recorded that "322 KB will not fit in 192 KB of DRAM" was
 * once given as the reason this project has no framebuffer, and that it was
 * false. It stays false, and it is now moot: tile hashing detects change in
 * 86 KB without storing a frame at all. See docs/DESIGN.md and tile.h. */
MEMORY
{
  iram (RWX) : ORIGIN = 0x40378000, LENGTH = 0x8000    /* 32K claimed,  ~14K used  */
  dram (RW)  : ORIGIN = 0x3FC90000, LENGTH = 0x50000   /* 320K claimed, ~205K used */
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
