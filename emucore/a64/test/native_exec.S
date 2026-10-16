/* native_exec.S - aarch64-only test oracle.
 *
 *   void native_exec(NativeState *st, const void *code);
 *
 * Loads guest registers from *st, sets SP, and branches to `code` with LR
 * pointing at native_done. The code is expected to finish with RET. native_done
 * copies the resulting registers / SP / NZCV back into *st.
 *
 * Reserved (not observable) registers: X16, X17 and X30.
 */

    .text
    .global native_exec
    .type native_exec, %function
native_exec:
    mov  x17, x1                 /* keep code pointer */

    /* Save the C caller's callee-saved registers. */
    stp x19, x20, [sp, #-96]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]

    adrp x16, g_osp
    add  x16, x16, :lo12:g_osp
    mov  x9, sp
    str  x9, [x16]               /* save wrapper SP */

    adrp x16, g_state
    add  x16, x16, :lo12:g_state
    str  x0, [x16]               /* save state pointer */

    /* load guest registers (x0 remains the base until last) */
    ldr x1,  [x0,#8]
    ldp x2,  x3,  [x0,#16]
    ldp x4,  x5,  [x0,#32]
    ldp x6,  x7,  [x0,#48]
    ldp x8,  x9,  [x0,#64]
    ldp x10, x11, [x0,#80]
    ldp x12, x13, [x0,#96]
    ldp x14, x15, [x0,#112]
    ldp x18, x19, [x0,#144]
    ldp x20, x21, [x0,#160]
    ldp x22, x23, [x0,#176]
    ldp x24, x25, [x0,#192]
    ldp x26, x27, [x0,#208]
    ldr x28, [x0,#224]
    ldr x29, [x0,#232]

    ldr x16, [x0,#248]           /* guest SP */
    mov sp, x16

    adrp x30, native_done
    add  x30, x30, :lo12:native_done
    ldr x0, [x0,#0]              /* x0 last */

    adrp x16, g_state
    add  x16, x16, :lo12:g_state
    ldr  x16, [x16]
    ldr  x16, [x16, #256]        /* initial NZCV (x16 is reserved) */
    msr  nzcv, x16
    br   x17
    .size native_exec, .-native_exec

    .type native_done, %function
native_done:
    adrp x16, g_state
    add  x16, x16, :lo12:g_state
    ldr  x16, [x16]              /* state pointer */

    stp x0,  x1,  [x16,#0]
    stp x2,  x3,  [x16,#16]
    stp x4,  x5,  [x16,#32]
    stp x6,  x7,  [x16,#48]
    stp x8,  x9,  [x16,#64]
    stp x10, x11, [x16,#80]
    stp x12, x13, [x16,#96]
    stp x14, x15, [x16,#112]
    stp x18, x19, [x16,#144]
    stp x20, x21, [x16,#160]
    stp x22, x23, [x16,#176]
    stp x24, x25, [x16,#192]
    stp x26, x27, [x16,#208]
    stp x28, x29, [x16,#224]

    mov  x9, sp
    str  x9, [x16,#248]          /* guest SP */
    mrs  x9, nzcv
    str  x9, [x16,#256]          /* NZCV */

    /* restore the C caller's callee-saved registers */
    adrp x16, g_osp
    add  x16, x16, :lo12:g_osp
    ldr  x16, [x16]
    mov  sp, x16
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #96
    ret
    .size native_done, .-native_done

    .bss
    .align 3
g_osp:   .quad 0
g_state: .quad 0

    .section .note.GNU-stack,"",%progbits
