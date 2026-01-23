.text
.globl mymalloc
.extern _mymalloc

mymalloc:
    xor %rax, %rax
    xor %rcx, %rcx
    xor %rdx, %rdx
    xor %rsi, %rsi
    xor %r8, %r8
    xor %r9, %r9
    xor %r10, %r10
    xor %r11, %r11
    
    push %rbp
    mov %rsp, %rbp
    
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    
    push $0x12abcdef
    sub $8, %rsp  
    
    call _mymalloc@PLT
    
    mov %rbp, %rsp
    pop %rbp
    ret