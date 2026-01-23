.text
.globl mymalloc
.globl runGC
.extern _mymalloc

mymalloc:
    # Nuke caller-saved registers except arguments (rdi holds Size)
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
    
    # Save callee-saved registers (potential roots)
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    
    # Stack marker and 16-byte alignment
    push $0x12abcdef
    sub $8, %rsp  
    
    # Correct way to call C functions in a shared library (-fPIC)
    call _mymalloc@PLT
    
    # Restore stack and return
    mov %rbp, %rsp
    pop %rbp
    ret

runGC:
    # satisfy the linker without needing a C function
    ret