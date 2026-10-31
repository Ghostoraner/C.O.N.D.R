bits 64

section .bss
align 16
stack_bottom:
    resb 64 * 1024              ; 64 KiB стек ядра
stack_top:

section .text
global _start
extern kernel_main

_start:
    cld                          ; SysV ABI вимагає DF=0
    mov rsp, stack_top
    xor rbp, rbp                 ; кінець ланцюжка фреймів
    push rbp                     ; вирівнювання стека під ABI
    push rbp

    call kernel_main

.hang:
    cli
    hlt
    jmp .hang