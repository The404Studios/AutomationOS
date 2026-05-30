/* Linker script for userspace programs.
 *
 * W^X layout: emit TWO PT_LOAD segments so the kernel loader can map code
 * read-only+executable and data read-write+no-execute independently:
 *
 *   text PT_LOAD (R+E, flags=5): .text + .rodata, based at 0x200000
 *   data PT_LOAD (R+W, flags=6): .data + .bss, on a FRESH page after text
 *
 * The data segment is page-aligned (0x1000) after the text segment so the two
 * never share a 4KB page — a shared page would force one permission on both and
 * defeat W^X. ENTRY stays _start; __bss_start/__bss_end remain accurate.
 */

ENTRY(_start)

/* Program headers: two loadable segments with explicit p_flags.
 *   PF_X=1, PF_W=2, PF_R=4  ->  text=R|E=5, data=R|W=6  */
PHDRS
{
    text PT_LOAD FLAGS(5);   /* R + E */
    data PT_LOAD FLAGS(6);   /* R + W */
}

SECTIONS
{
    /* Load userspace at 2MB (0x200000) */
    . = 0x200000;

    /* ---- text segment (RX): code + read-only data ---- */
    .text : {
        *(.text .text.*)
    } :text

    .rodata : {
        *(.rodata .rodata.*)
    } :text

    /* Start the data segment on a fresh page so it never shares a 4KB page
     * with the text segment (W^X requires distinct pages per permission). */
    . = ALIGN(0x1000);

    /* ---- data segment (RW + NX): writable globals + BSS ---- */
    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } :data

    /* Discard unnecessary sections */
    /DISCARD/ : {
        *(.note.GNU-stack)
        *(.comment)
        *(.eh_frame)
    }
}
