; fault_handler_asm.s — HardFault assembly trampoline for IAR EWARM
;
; Captures the faulting stack pointer BEFORE the C compiler's prologue
; can modify it, then calls hard_fault_handler_c(uint32_t* sp).
;
; EXC_RETURN bit 2:
;   0 → faulting context used MSP (handler / ISR mode)
;   1 → faulting context used PSP (thread mode)
;
; For INVPC (bad exception return from an ISR), bit 2 = 0, so MSP is used.
; The stacked frame at sp[] holds: R0,R1,R2,R3,R12,LR,PC,xPSR.
; sp[5] = the invalid EXC_RETURN that caused INVPC.
; sp[6] = PC of the BX LR instruction in the faulting ISR.

        MODULE  fault_handler_asm

        SECTION .text:CODE:NOROOT(2)
        THUMB

        EXTERN  hard_fault_handler_c
        PUBLIC  HardFault_Handler

HardFault_Handler
        TST     LR, #4          ; test bit 2 of EXC_RETURN in LR
        ITE     EQ
        MRSEQ   R0, MSP         ; bit2=0 → handler mode → use MSP
        MRSNE   R0, PSP         ; bit2=1 → thread  mode → use PSP
        B       hard_fault_handler_c   ; tail-call: R0 = first argument

        END
