bits 64
default rel
global _start

section .text
_start:
    sub rsp, 64

    mov rax, 3
    lea rdi, [service_name]
    mov rsi, service_name_end - service_name
    int 0x80
    test rax, rax
    js .exit_failure
    mov [rsp], rax

    mov rax, 4
    lea rdi, [service_name]
    mov rsi, service_name_end - service_name
    int 0x80
    test rax, rax
    js .exit_failure
    mov [rsp + 8], rax

    mov rax, 5
    mov rdi, [rsp]
    int 0x80
    test rax, rax
    js .exit_failure
    mov [rsp + 16], rax

    mov rax, 6
    mov rdi, [rsp + 8]
    lea rsi, [request]
    mov rdx, request_end - request
    int 0x80
    test rax, rax
    js .exit_failure

    mov rax, 7
    mov rdi, [rsp + 16]
    lea rsi, [rsp + 32]
    mov rdx, 64
    int 0x80
    test rax, rax
    js .exit_failure

    mov rax, 6
    mov rdi, [rsp + 16]
    lea rsi, [response]
    mov rdx, response_end - response
    int 0x80
    test rax, rax
    js .exit_failure

    mov rax, 7
    mov rdi, [rsp + 8]
    lea rsi, [rsp + 32]
    mov rdx, 64
    int 0x80
    test rax, rax
    js .exit_failure
    mov rsi, rax

    mov rax, 1
    lea rdi, [success_message]
    mov rsi, success_message_end - success_message
    int 0x80

    mov rax, 1
    lea rdi, [rsp + 32]
    mov rsi, 4
    int 0x80

    mov rax, 1
    lea rdi, [newline]
    mov rsi, 1
    int 0x80

    mov rax, 2
    xor rdi, rdi
    int 0x80

.exit_failure:
    mov rax, 2
    mov rdi, 1
    int 0x80
    jmp $

    jmp $

section .rodata
service_name: db "echo"
service_name_end:
request: db "ping"
request_end:
response: db "pong"
response_end:
success_message: db "C.O.N.D.R user-mode IPC: "
success_message_end:
newline: db 10

section .note.GNU-stack