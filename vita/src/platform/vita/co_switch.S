.syntax unified
.cpu cortex-a9
.fpu neon
.text
.align 4

@ Cooperative context switch for Vita ARMv7 hard-float (AAPCS).
@ r0 = address of the target saved SP
@ r1 = address that receives the current saved SP
@ Frame is 128 bytes: r4-r11, lr, pad, d8-d15.
.global mkw_co_switch
.type mkw_co_switch, %function
mkw_co_switch:
    sub sp, sp, #128
    stmia sp, {r4-r11}
    str lr, [sp, #32]
    add r2, sp, #40
    vstmia r2, {d8-d15}
    str sp, [r1]
    ldr sp, [r0]
    add r2, sp, #40
    vldmia r2, {d8-d15}
    ldr lr, [sp, #32]
    ldmia sp, {r4-r11}
    add sp, sp, #128
    bx lr

@ r0 = one-past-end of the new stack
@ r1 = entry(void *)
@ r2 = entry argument
@ returns the initial saved SP in r0
.global mkw_co_init
.type mkw_co_init, %function
mkw_co_init:
    bic r0, r0, #15
    sub r0, r0, #128
    str r1, [r0, #0]          @ r4 = entry
    str r2, [r0, #4]          @ r5 = argument
    adr r3, mkw_co_entry_trampoline
    str r3, [r0, #32]         @ lr
    bx lr

.type mkw_co_entry_trampoline, %function
mkw_co_entry_trampoline:
    mov r0, r5
    blx r4
    bkpt #0
