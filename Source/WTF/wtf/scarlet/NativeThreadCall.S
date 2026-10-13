#if !defined(__scarlet__) || !defined(__aarch64__)
#error "NativeThreadCall requires Scarlet on AArch64"
#endif

.text
.globl scarlet_native_call
.type scarlet_native_call,%function
scarlet_native_call:
    mov x8,x0
    movk x8,#0x5343,lsl #16
    mov x0,x1
    mov x1,x2
    mov x2,x3
    mov x3,x4
    mov x4,x5
    svc #0
    ret
.size scarlet_native_call,.-scarlet_native_call
.globl scarlet_gc_event_return
.type scarlet_gc_event_return,%function
scarlet_gc_event_return:
    mov x8,#643
    movk x8,#0x5343,lsl #16
    svc #0
    brk #0
.size scarlet_gc_event_return,.-scarlet_gc_event_return
.globl scarlet_gc_event_entry
.type scarlet_gc_event_entry,%function
scarlet_gc_event_entry:
    sub sp,sp,#544
    stp q0,q1,[sp,#0]
    stp q2,q3,[sp,#32]
    stp q4,q5,[sp,#64]
    stp q6,q7,[sp,#96]
    stp q8,q9,[sp,#128]
    stp q10,q11,[sp,#160]
    stp q12,q13,[sp,#192]
    stp q14,q15,[sp,#224]
    stp q16,q17,[sp,#256]
    stp q18,q19,[sp,#288]
    stp q20,q21,[sp,#320]
    stp q22,q23,[sp,#352]
    stp q24,q25,[sp,#384]
    stp q26,q27,[sp,#416]
    stp q28,q29,[sp,#448]
    stp q30,q31,[sp,#480]
    mrs x4,fpcr
    mrs x5,fpsr
    str x4,[sp,#512]
    str x5,[sp,#520]
    str x30,[sp,#528]
    mov x3,sp
    bl scarlet_gc_event_handler
    ldp q0,q1,[sp,#0]
    ldp q2,q3,[sp,#32]
    ldp q4,q5,[sp,#64]
    ldp q6,q7,[sp,#96]
    ldp q8,q9,[sp,#128]
    ldp q10,q11,[sp,#160]
    ldp q12,q13,[sp,#192]
    ldp q14,q15,[sp,#224]
    ldp q16,q17,[sp,#256]
    ldp q18,q19,[sp,#288]
    ldp q20,q21,[sp,#320]
    ldp q22,q23,[sp,#352]
    ldp q24,q25,[sp,#384]
    ldp q26,q27,[sp,#416]
    ldp q28,q29,[sp,#448]
    ldp q30,q31,[sp,#480]
    ldr x4,[sp,#512]
    ldr x5,[sp,#520]
    msr fpcr,x4
    msr fpsr,x5
    ldr x30,[sp,#528]
    add sp,sp,#544
    ret
.size scarlet_gc_event_entry,.-scarlet_gc_event_entry
.section .note.GNU-stack,"",%progbits
