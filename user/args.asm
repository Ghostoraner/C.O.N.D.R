bits 64
default rel
global _start

; Prints every argv and envp entry, then exits with argc as the status. The
; spawn test uses the status to confirm that the System V initial stack was
; built with the expected number of arguments.
section .text
_start:
    mov r12, [rsp]              ; argc
    lea r13, [rsp + 8]          ; argv

    xor r15, r15
.argv_loop:
    cmp r15, r12
    jae .env_start
    mov rdi, [r13 + r15 * 8]
    call print_line
    inc r15
    jmp .argv_loop

.env_start:
    lea r13, [r13 + r12 * 8 + 8] ; skip the argv NULL terminator
.env_loop:
    mov rdi, [r13]
    test rdi, rdi
    jz .done
    call print_line
    add r13, 8
    jmp .env_loop

.done:
    mov rax, 2                  ; exit(argc)
    mov rdi, r12
    int 0x80
    jmp $

print_line:
    push rbx
    mov rbx, rdi
    xor rsi, rsi
.length:
    cmp byte [rbx + rsi], 0
    je .write
    inc rsi
    jmp .length
.write:
    mov rax, 1                  ; write(rdi, rsi)
    int 0x80
    mov rax, 1
    lea rdi, [newline]
    mov rsi, 1
    int 0x80
    pop rbx
    ret

section .rodata
newline: db 10

section .note.GNU-stack noalloc noexec nowrite progbits
