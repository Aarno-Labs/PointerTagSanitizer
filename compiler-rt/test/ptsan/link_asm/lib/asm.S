// asm_sum_two_x86_64.S
    .text
    .globl asm_sum_two
    .type  asm_sum_two, @function
asm_sum_two:
    # int asm_sum_two(const int *p)  ; p in %rdi, return in %eax
    movl    (%rdi), %eax        # load p[0]
    addl    4(%rdi), %eax       # add  p[1]
    ret
    .size asm_sum_two, .-asm_sum_two

    .section .note.GNU-stack,"",@progbits