/* Deliberately no CFI: Linux uses ORC hints here, not GDB's DWARF CFI. */
.text
.globl asm_exc_divide_error
.type asm_exc_divide_error,@function
asm_exc_divide_error:
    mov %rsp, %rdi
#ifdef TEST_ERROR_CODE
    mov 120(%rsp), %rsi
    movq $-1, 120(%rsp)
#endif
#ifdef TEST_UNSUPPORTED_ENTRY
    push %rax
#endif
    call exc_divide_error
    ud2
.size asm_exc_divide_error, .-asm_exc_divide_error
.section .note.GNU-stack,"",@progbits
