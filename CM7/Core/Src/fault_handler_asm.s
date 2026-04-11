; fault_handler_asm.s — placeholder (custom HardFault trampoline disabled)
;
; Original code removed to avoid duplicate symbol conflict with
; HardFault_Handler in stm32h7xx_it.c.
; To re-enable: restore original code, remove HardFault_Handler from
; stm32h7xx_it.c, and add hard_fault_handler_c() in a .c file.

        MODULE  fault_handler_asm
        SECTION .text:CODE:NOROOT(2)
        THUMB
        ; intentionally empty — keeps linker happy
        END
