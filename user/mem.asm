bits 64
default rel
global _start

; Exercises the user-memory syscalls: brk() grows a demand-zero heap, mmap()
; creates anonymous demand-zero pages, and both regions are touched so the
; kernel page-fault path materializes them. Exits with status 0 on success.
section .text
_start:
    mov rax, 13                 ; brk(0) -> current break
    xor rdi, rdi
    int 0x80
    test rax, rax
    js .fail
    mov r12, rax

    lea rdi, [r12 + 0x3000]     ; grow the heap by three pages
    mov rax, 13
    int 0x80
    test rax, rax
    js .fail
    mov rcx, r12
    add rcx, 0x3000
    cmp rax, rcx
    jb .fail

    mov rax, 0x1122334455667788 ; touch the first heap page
    mov [r12], rax
    cmp [r12], rax
    jne .fail

    mov rax, 0x8877665544332211 ; touch the third heap page
    mov [r12 + 0x2000], rax
    cmp [r12 + 0x2000], rax
    jne .fail

    mov rax, 14                 ; mmap(NULL, 0x2000, RW, PRIVATE|ANON)
    xor rdi, rdi
    mov rsi, 0x2000
    mov rdx, 3
    mov r10, 0x22
    int 0x80
    test rax, rax
    js .fail
    mov r13, rax

    cmp qword [r13], 0          ; untouched anonymous memory is zero-filled
    jne .fail

    mov rax, 0xDEADBEEFCAFEBABE ; write through the second mmap page
    mov [r13 + 0x1000], rax
    cmp [r13 + 0x1000], rax
    jne .fail

    mov rax, 14                 ; a second mmap must return a fresh range
    xor rdi, rdi
    mov rsi, 0x1000
    mov rdx, 3
    mov r10, 0x22
    int 0x80
    test rax, rax
    js .fail
    cmp rax, r13
    je .fail

    mov rax, 1                  ; write(1, message, len)
    lea rdi, [message]
    mov rsi, message_end - message
    int 0x80

    mov rax, 2                  ; exit(0)
    xor rdi, rdi
    int 0x80
    jmp $

.fail:
    mov rax, 2                  ; exit(1)
    mov rdi, 1
    int 0x80
    jmp $

section .rodata
message: db "C.O.N.D.R user memory: brk/mmap ok", 10
message_end:

section .note.GNU-stack noalloc noexec nowrite progbits
