bits 64
default rel
global _start

section .text
_start:
    sub rsp, 64

    ; Take keyboard focus (syscall 9, rdi = 1). Console input is suspended
    ; until this program gives focus back or exits.
    mov rax, 9
    mov rdi, 1
    int 0x80
    cmp rax, 0
    jne .read_failed

    ; Announce ourselves on the console (syscall 1).
    mov rax, 1
    lea rdi, [msg_ready]
    mov rsi, msg_ready_end - msg_ready
    int 0x80

    mov ecx, 5
.read:
    mov rax, 8            ; sys_read_char
    lea rdi, [rsp]        ; user buffer
    mov rsi, 1            ; capacity (one character per call)
    int 0x80
    cmp rax, 1
    jne .read_failed

    ; Echo the character back (syscall 1).
    mov rax, 1
    lea rdi, [rsp]
    mov rsi, 1
    int 0x80

    dec ecx
    jnz .read

    ; Return keyboard focus to the console (syscall 9, rdi = 0).
    mov rax, 9
    xor rdi, rdi
    int 0x80

    mov rax, 1
    lea rdi, [msg_done]
    mov rsi, msg_done_end - msg_done
    int 0x80

    mov rax, 2
    xor rdi, rdi
    int 0x80
    jmp $

.read_failed:
    mov rax, 1
    lea rdi, [msg_error]
    mov rsi, msg_error_end - msg_error
    int 0x80
    mov rax, 2
    mov rdi, 1
    int 0x80
    jmp $

section .rodata
msg_ready: db "kbd-demo: ready (5 chars)", 10
msg_ready_end:
msg_error: db "kbd-demo: read failed", 10
msg_error_end:
msg_done: db "kbd-demo: done", 10
msg_done_end:

section .note.GNU-stack noalloc noexec nowrite progbits