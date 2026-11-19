#include "../common/newminhh_abi.h"

.text
.p2align 4

/*
 * uint64_t newminhh_call_x32_sig(
 *     void *target_x32_fn,          // %edi
 *     const nm_fn_sig_t *sig,       // %esi
 *     const uint32_t *cdecl_args,   // %edx
 *     uint64_t *out_fp_bits         // %ecx (may be NULL)
 * );
 */
.globl newminhh_call_x32_sig
.type newminhh_call_x32_sig, @function
newminhh_call_x32_sig:
    pushq   %rbp
    movq    %rsp, %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    movl    %edi, %r12d             /* %r12 = target_x32_fn */
    movl    %esi, %r13d             /* %r13 = sig */
    movl    %edx, %r15d             /* %r15 = cdecl_args cursor */
    movl    %ecx, %r14d             /* %r14 = out_fp_bits */

    /* Frame: 5 pushes (40B) + 248B = 288B -> %rsp is 16B aligned */
    subq    $248, %rsp
    /*
     * [%rsp +   0 ..  47] = gprs[0..5]
     * [%rsp +  48 .. 111] = xmms[0..7]
     * [%rsp + 112 .. 239] = spills[0..15]
     */

    xorl    %ebx, %ebx              /* arg index i = 0 */
    xorl    %ecx, %ecx              /* gpr_idx = 0 */
    xorl    %edx, %edx              /* xmm_idx = 0 */
    xorl    %r8d, %r8d              /* spill_idx = 0 */

    movzbl  1(%r13), %r11d          /* sig->nargs */
    cmpl    $NM_MAX_SIG_ARGS, %r11d
    jbe     .Lcx_loop_check
    movl    $NM_MAX_SIG_ARGS, %r11d

.Lcx_loop_check:
    cmpl    %r11d, %ebx
    jge     .Lcx_loop_done

    movzbl  4(%r13, %rbx, 1), %r9d  /* arg_kind */

    cmpl    $NM_ARG_I32, %r9d
    je      .Lcx_arg_i32
    cmpl    $NM_ARG_U32, %r9d
    je      .Lcx_arg_u32
    cmpl    $NM_ARG_PTR, %r9d
    je      .Lcx_arg_u32
    cmpl    $NM_ARG_I64, %r9d
    je      .Lcx_arg_i64
    cmpl    $NM_ARG_U64, %r9d
    je      .Lcx_arg_i64
    cmpl    $NM_ARG_F32, %r9d
    je      .Lcx_arg_f32
    cmpl    $NM_ARG_F64, %r9d
    je      .Lcx_arg_f64
    jmp     .Lcx_next

.Lcx_arg_i32:
    movslq  (%r15), %rax
    addq    $4, %r15
    jmp     .Lcx_store_gpr

.Lcx_arg_u32:
    movl    (%r15), %eax
    addq    $4, %r15
    jmp     .Lcx_store_gpr

.Lcx_arg_i64:
    movq    (%r15), %rax
    addq    $8, %r15

.Lcx_store_gpr:
    cmpl    $6, %ecx
    jge     .Lcx_store_spill
    movq    %rax, (%rsp, %rcx, 8)
    incl    %ecx
    jmp     .Lcx_next

.Lcx_arg_f32:
    movl    (%r15), %eax
    addq    $4, %r15
    jmp     .Lcx_store_xmm

.Lcx_arg_f64:
    movq    (%r15), %rax
    addq    $8, %r15

.Lcx_store_xmm:
    cmpl    $8, %edx
    jge     .Lcx_store_spill
    movq    %rax, 48(%rsp, %rdx, 8)
    incl    %edx
    jmp     .Lcx_next

.Lcx_store_spill:
    cmpl    $16, %r8d
    jge     .Lcx_next
    movq    %rax, 112(%rsp, %r8, 8)
    incl    %r8d

.Lcx_next:
    incl    %ebx
    jmp     .Lcx_loop_check

.Lcx_loop_done:
    movq    %rsp, %r10              /* %r10 = saved local buffer base */
    movl    %edx, %r15d             /* %r15d = xmm_count */

    testl   %r8d, %r8d
    jz      .Lcx_no_spill

    /* Align spill_count up to even number of 8-byte slots */
    movl    %r8d, %eax
    addl    $1, %eax
    andl    $-2, %eax
    shll    $3, %eax                /* bytes to subtract from %rsp */
    subq    %rax, %rsp

    xorl    %ebx, %ebx
.Lcx_spill_copy:
    cmpl    %r8d, %ebx
    jge     .Lcx_no_spill
    movq    112(%r10, %rbx, 8), %rax
    movq    %rax, (%rsp, %rbx, 8)
    incl    %ebx
    jmp     .Lcx_spill_copy

.Lcx_no_spill:
    testl   %r15d, %r15d
    jz      .Lcx_load_gprs
    movsd   48(%r10), %xmm0
    movsd   56(%r10), %xmm1
    movsd   64(%r10), %xmm2
    movsd   72(%r10), %xmm3
    movsd   80(%r10), %xmm4
    movsd   88(%r10), %xmm5
    movsd   96(%r10), %xmm6
    movsd   104(%r10), %xmm7

.Lcx_load_gprs:
    movq    0(%r10), %rdi
    movq    8(%r10), %rsi
    movq    16(%r10), %rdx
    movq    24(%r10), %rcx
    movq    32(%r10), %r8
    movq    40(%r10), %r9

    movl    %r15d, %eax             /* %al = xmm_count for variadic ABI */
    call    *%r12

    leaq    -40(%rbp), %rsp         /* restore rsp past spill + 248B buffer */

    testq   %r14, %r14
    jz      .Lcx_done
    movzbl  0(%r13), %ecx           /* sig->ret_kind */
    cmpl    $NM_RET_F32, %ecx
    je      .Lcx_store_f32
    cmpl    $NM_RET_F64, %ecx
    je      .Lcx_store_f64
    jmp     .Lcx_done

.Lcx_store_f32:
    movq    $0, (%r14)
    movss   %xmm0, (%r14)
    jmp     .Lcx_done

.Lcx_store_f64:
    movsd   %xmm0, (%r14)

.Lcx_done:
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret
.size newminhh_call_x32_sig, .-newminhh_call_x32_sig

/*
 * uint64_t newminhh_call_x32_cdecl(
 *     void *target_x32_fn,          // %edi
 *     const nm_fn_sig_t *sig,       // %esi
 *     const uint32_t *cdecl_args    // %edx
 * );
 * Returns EAX / EDX:EAX / x87 ST(0) according to sig->ret_kind.
 */
.globl newminhh_call_x32_cdecl
.type newminhh_call_x32_cdecl, @function
newminhh_call_x32_cdecl:
    pushq   %rbx
    subq    $16, %rsp               /* 1 push (8B) + 16B = 24B -> 16B aligned */
    movl    %esi, %ebx              /* save sig */
    movq    %rsp, %rcx              /* out_fp_bits = &rsp[0] */
    call    newminhh_call_x32_sig

    movzbl  0(%rbx), %ecx           /* ret_kind */
    cmpl    $NM_RET_I64, %ecx
    je      .Lcdecl_ret64
    cmpl    $NM_RET_U64, %ecx
    je      .Lcdecl_ret64
    cmpl    $NM_RET_F32, %ecx
    je      .Lcdecl_ret_f32
    cmpl    $NM_RET_F64, %ecx
    je      .Lcdecl_ret_f64
    cmpl    $NM_RET_VOID, %ecx
    je      .Lcdecl_ret_done
    movl    %eax, %eax
    jmp     .Lcdecl_ret_done

.Lcdecl_ret64:
    movq    %rax, %rdx
    shrq    $32, %rdx
    movl    %eax, %eax
    jmp     .Lcdecl_ret_done

.Lcdecl_ret_f32:
    flds    (%rsp)
    jmp     .Lcdecl_ret_done

.Lcdecl_ret_f64:
    fldl    (%rsp)

.Lcdecl_ret_done:
    addq    $16, %rsp
    popq    %rbx
    ret
.size newminhh_call_x32_cdecl, .-newminhh_call_x32_cdecl

/*
 * Outgoing trampoline common entry:
 * Invoked via 16B pool slot: movl $slot_id, %r11d; jmp newminhh_out_common
 * Caller uses i386 4-byte stack stride: [rsp+0] = 4B return addr, [rsp+4] = arg0, ...
 */
.globl newminhh_out_common
.type newminhh_out_common, @function
newminhh_out_common:
    movq    %rsp, %rax              /* original guest RSP */
    andq    $-16, %rsp              /* align host stack to 16B */
    subq    $32, %rsp               /* 32B local area (16B aligned) */
    /*
     * Local area relative to %rsp before pushes:
     *   0(%rsp)  (8B): saved original guest RSP
     *   8(%rsp)  (8B): uint64_t out_val
     *  16(%rsp)  (8B): uint64_t out_fp_bits
     *  24(%rsp)  (4B): uint32_t out_ret_kind
     *  28(%rsp)  (4B): padding
     */
    movq    %rax, 0(%rsp)           /* save original guest RSP */
    movq    $0, 8(%rsp)             /* out_val = 0 */
    movq    $0, 16(%rsp)            /* out_fp_bits = 0 */
    movl    $0, 24(%rsp)            /* out_ret_kind = 0 */
    pushq   %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13                    /* 4 pushes (32B) + 32B = 64B -> 16B aligned */

    movq    %rax, %r12              /* %r12 = guest RSP */
    movl    %r11d, %edi             /* arg1: slot_id */
    leaq    4(%r12), %rsi           /* arg2: cdecl_args = guest RSP + 4 */
    leaq    40(%rsp), %rdx          /* arg3: &out_val */
    leaq    48(%rsp), %rcx          /* arg4: &out_fp_bits */
    leaq    56(%rsp), %r8           /* arg5: &out_ret_kind */
    call    newminhh_out_dispatch

    movq    40(%rsp), %rax          /* load out_val */
    movl    56(%rsp), %ecx          /* load out_ret_kind */
    cmpl    $NM_RET_I64, %ecx
    je      .Lout_ret64
    cmpl    $NM_RET_U64, %ecx
    je      .Lout_ret64
    cmpl    $NM_RET_F32, %ecx
    je      .Lout_ret_f32
    cmpl    $NM_RET_F64, %ecx
    je      .Lout_ret_f64
    cmpl    $NM_RET_VOID, %ecx
    je      .Lout_ret_void
    movl    %eax, %eax              /* 32-bit I32/U32/PTR return in EAX */
    jmp     .Lout_ret_done

.Lout_ret_void:
    xorl    %eax, %eax
    jmp     .Lout_ret_done

.Lout_ret64:
    movq    %rax, %rdx
    shrq    $32, %rdx               /* high 32 bits in EDX */
    movl    %eax, %eax              /* low 32 bits in EAX */
    jmp     .Lout_ret_done

.Lout_ret_f32:
    flds    48(%rsp)                /* load float into x87 ST(0) */
    jmp     .Lout_ret_done

.Lout_ret_f64:
    fldl    48(%rsp)                /* load double into x87 ST(0) */

.Lout_ret_done:
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    movq    0(%rsp), %rsp           /* restore original guest RSP */
    movl    (%rsp), %r11d           /* 4-byte ret addr */
    leaq    4(%rsp), %rsp
    jmp     *%r11
.size newminhh_out_common, .-newminhh_out_common

/*
 * uint64_t newminhh_invoke_cdecl_asm(
 *     uintptr_t target_fn,          // %edi
 *     const uint32_t *cdecl_words,  // %esi
 *     int nwords,                   // %edx
 *     uint8_t ret_kind,             // %ecx
 *     double *out_fp                // %r8 (may be NULL)
 * );
 * Invokes a Long Mode cdecl-stride callee where [esp+0]=4B ret addr, [esp+4]=arg0, ...
 * and callee returns via movl (%rsp), %r11d; lea 4(%rsp), %rsp; jmp *%r11.
 */
.globl newminhh_invoke_cdecl_asm
.type newminhh_invoke_cdecl_asm, @function
newminhh_invoke_cdecl_asm:
    pushq   %rbp
    movq    %rsp, %rbp
    pushq   %rbx
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15

    movq    %rsp, %r12              /* save original RSP for restore */
    movl    %edi, %r13d             /* target_fn */
    movzbl  %cl, %r14d              /* ret_kind */
    movl    %r8d, %r15d             /* out_fp */

    andq    $-16, %rsp              /* align %rsp to 16B */

    /* Allocate (((nwords * 4) + 15) & -16) + 4 bytes so %rsp % 16 == 12 and 4(%rsp) % 16 == 0 */
    movl    %edx, %eax
    testl   %eax, %eax
    jns     .Lic_nonneg
    xorl    %eax, %eax
.Lic_nonneg:
    movl    %eax, %ecx              /* nwords */
    shll    $2, %eax                /* nwords * 4 */
    addl    $15, %eax
    andl    $-16, %eax              /* round arg area up to 16B */
    addl    $4, %eax                /* +4 for 4-byte return address */
    subq    %rax, %rsp

    /* Copy cdecl_words[0..nwords-1] to 4(%rsp, i*4) */
    xorl    %ebx, %ebx
.Lic_copy:
    cmpl    %ecx, %ebx
    jge     .Lic_copy_done
    movl    (%rsi, %rbx, 4), %eax
    movl    %eax, 4(%rsp, %rbx, 4)
    incl    %ebx
    jmp     .Lic_copy

.Lic_copy_done:
    leal    .Lic_ret_addr(%rip), %eax
    movl    %eax, (%rsp)            /* 4-byte return address */
    jmp     *%r13

.Lic_ret_addr:
    movq    %r12, %rsp              /* restore stack regardless of callee cleanup */

    cmpl    $NM_RET_VOID, %r14d
    je      .Lic_ret_void
    cmpl    $NM_RET_I32, %r14d
    je      .Lic_ret_i32
    cmpl    $NM_RET_U32, %r14d
    je      .Lic_ret_u32
    cmpl    $NM_RET_PTR, %r14d
    je      .Lic_ret_u32
    cmpl    $NM_RET_I64, %r14d
    je      .Lic_ret_i64
    cmpl    $NM_RET_U64, %r14d
    je      .Lic_ret_i64
    cmpl    $NM_RET_F32, %r14d
    je      .Lic_ret_fp
    cmpl    $NM_RET_F64, %r14d
    je      .Lic_ret_fp

.Lic_ret_void:
    xorl    %eax, %eax
    jmp     .Lic_done

.Lic_ret_i32:
    movslq  %eax, %rax
    jmp     .Lic_done

.Lic_ret_u32:
    movl    %eax, %eax
    jmp     .Lic_done

.Lic_ret_i64:
    movl    %eax, %eax
    shlq    $32, %rdx
    orq     %rdx, %rax
    jmp     .Lic_done

.Lic_ret_fp:
    testq   %r15, %r15
    jz      .Lic_ret_fp_pop
    fstpl   (%r15)
    xorl    %eax, %eax
    jmp     .Lic_done
.Lic_ret_fp_pop:
    fstp    %st(0)
    xorl    %eax, %eax

.Lic_done:
    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbx
    popq    %rbp
    ret
.size newminhh_invoke_cdecl_asm, .-newminhh_invoke_cdecl_asm

/*
 * Incoming x32 -> i386 callback / export trampoline common handler.
 * Entered via 16B pool slot:
 *   movl $slot_id, %r11d
 *   jmp  newminhh_cb_common
 */
.globl newminhh_cb_common
.type newminhh_cb_common, @function
newminhh_cb_common:
    leaq    8(%rsp), %r10           /* caller stack-spill pointer */
    pushq   %rbp
    movq    %rsp, %rbp              /* %rsp is now 16B aligned */
    subq    $128, %rsp

    /* Save x32 GPR arg regs: gprs[0..5] at [%rsp + 0..47] */
    movq    %rdi,  0(%rsp)
    movq    %rsi,  8(%rsp)
    movq    %rdx, 16(%rsp)
    movq    %rcx, 24(%rsp)
    movq    %r8,  32(%rsp)
    movq    %r9,  40(%rsp)

    /* Save x32 XMM arg regs: xmms[0..7] at [%rsp + 48..111] */
    movsd   %xmm0,  48(%rsp)
    movsd   %xmm1,  56(%rsp)
    movsd   %xmm2,  64(%rsp)
    movsd   %xmm3,  72(%rsp)
    movsd   %xmm4,  80(%rsp)
    movsd   %xmm5,  88(%rsp)
    movsd   %xmm6,  96(%rsp)
    movsd   %xmm7, 104(%rsp)

    movq    $0, 112(%rsp)           /* double out_fp = 0.0 */
    movl    $0, 120(%rsp)           /* uint32_t out_ret_kind = 0 */

    movl    %r11d, %edi             /* slot_id */
    movq    %rsp, %rsi              /* gprs */
    leaq    48(%rsp), %rdx          /* xmms */
    movq    %r10, %rcx              /* caller_stack */
    leaq    112(%rsp), %r8          /* &out_fp */
    leaq    120(%rsp), %r9          /* &out_ret_kind */
    call    newminhh_cb_dispatch

    movl    120(%rsp), %ecx
    cmpl    $NM_RET_F32, %ecx
    je      .Lcb_ret_f32
    cmpl    $NM_RET_F64, %ecx
    je      .Lcb_ret_f64
    jmp     .Lcb_done

.Lcb_ret_f32:
    cvtsd2ss 112(%rsp), %xmm0
    jmp     .Lcb_done

.Lcb_ret_f64:
    movsd   112(%rsp), %xmm0

.Lcb_done:
    leave
    ret
.size newminhh_cb_common, .-newminhh_cb_common

.section .note.GNU-stack,"",@progbits
