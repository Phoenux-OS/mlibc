.section .phoenux_entry
.global _start
_start:
    jmp _start1

.section .text
_start1:
    push $main
    call __mlibc_entry

.global ___tls_get_addr
___tls_get_addr:
    mov $0x12348765, %eax
    jmp ___tls_get_addr
