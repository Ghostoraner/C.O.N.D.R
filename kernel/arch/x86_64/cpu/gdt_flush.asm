bits 64
section .text
global gdt_flush

gdt_flush:
    lgdt [rdi]

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    pop rax
    push qword 0x08
    push rax
    retfq