; Minimal ring 3 test: just an infinite loop
; If this runs without triple fault, ring 3 transition works
global _start

section .text
_start:
.loop:
    jmp .loop
