/*
 * flipper-doom: the engine image that the FAP installs into the unused
 * internal flash gap and runs in place.
 *
 * Code and constants are linked at their final flash address. Mutable data is
 * linked at a placeholder RAM address and reached through the GOT (r9); the
 * loader copies .got/.data to a heap block and relocates them, see mkblob.py.
 * The first 64 bytes of the gap hold the image header written by mkblob.py.
 */

MEMORY
{
    /* the real limit (104K gap) is enforced by mkblob.py, which can report why */
    FLASH (rx) : ORIGIN = 0x080BD040, LENGTH = 256K
    RAM (rw)   : ORIGIN = 0x10000000, LENGTH = 128K
}

ENTRY(fz_engine_main)

SECTIONS
{
    .text :
    {
        KEEP(*(.text.fz_engine_main))
        *(.text .text.*)
    } > FLASH

    .rodata :
    {
        *(.rodata .rodata.*)
        /* const data with pointers: fine in flash as long as nothing points to RAM (checked by mkblob.py) */
        *(.data.rel.ro .data.rel.ro.*)
        . = ALIGN(4);
    } > FLASH

    .got :
    {
        __ram_start = .;
        *(.got.plt) *(.igot.plt) *(.got) *(.igot)
    } > RAM AT > FLASH

    .data :
    {
        *(.data .data.*)
        . = ALIGN(4);
        __data_end = .;
    } > RAM AT > FLASH

    __ram_load = LOADADDR(.got);

    .bss (NOLOAD) :
    {
        *(.bss .bss.*) *(COMMON)
        . = ALIGN(4);
        __bss_end = .;
    } > RAM

    /DISCARD/ :
    {
        *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.init) *(.fini)
    }
}
