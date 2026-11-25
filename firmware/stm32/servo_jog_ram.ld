ENTRY(entry)
SECTIONS {
  . = 0x20008000;
  .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) }
  .bss (NOLOAD) : { . = ALIGN(4); _bss_start = .; *(.bss*) *(COMMON) . = ALIGN(4); _bss_end = .; }
  /DISCARD/ : { *(.ARM.exidx*) *(.comment*) }
}\n