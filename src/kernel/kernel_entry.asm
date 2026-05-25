; ==========================================
; QuantumOS Kernel Entry (Fixed)
; FPU init, ISR stubs, I/O helpers
; ==========================================

SECTION .text
[bits 32]

global _start
extern _kernel_main

_start:
    ; Initialize the x87 FPU - CRITICAL: prevents #NM exceptions
    fninit

    call _kernel_main

.halt_loop:
    cli
    hlt
    jmp .halt_loop

; --- I/O Port Helpers ---
global _inb
_inb:
    push ebp
    mov ebp, esp
    mov dx, [ebp + 8]
    xor eax, eax
    in al, dx
    pop ebp
    ret

global _outb
_outb:
    push ebp
    mov ebp, esp
    mov dx, [ebp + 8]
    mov al, [ebp + 12]
    out dx, al
    pop ebp
    ret

global _inw
_inw:
    push ebp
    mov ebp, esp
    mov dx, [ebp + 8]
    xor eax, eax
    in ax, dx
    pop ebp
    ret

global _outw
_outw:
    push ebp
    mov ebp, esp
    mov dx, [ebp + 8]
    mov ax, [ebp + 12]
    out dx, ax
    pop ebp
    ret

global _inl
_inl:
    push ebp
    mov ebp, esp
    mov dx, [ebp + 8]
    in eax, dx
    pop ebp
    ret

global _outl
_outl:
    push ebp
    mov ebp, esp
    mov dx, [ebp + 8]
    mov eax, [ebp + 12]
    out dx, eax
    pop ebp
    ret

global _lidt
_lidt:
    push ebp
    mov ebp, esp
    mov eax, [ebp + 8]
    lidt [eax]
    pop ebp
    ret

global _enable_interrupts
_enable_interrupts:
    sti
    ret

global _disable_interrupts
_disable_interrupts:
    cli
    ret

; --- IRQ Handlers ---

global _timer_handler_asm
extern _timer_handler
_timer_handler_asm:
    pusha
    call _timer_handler
    mov al, 0x20
    out 0x20, al
    popa
    iret

global _keyboard_handler_asm
extern _keyboard_handler
_keyboard_handler_asm:
    pusha
    call _keyboard_handler
    mov al, 0x20
    out 0x20, al
    popa
    iret

global _mouse_handler_asm
extern _mouse_handler
_mouse_handler_asm:
    pusha
    call _mouse_handler
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    popa
    iret

; --- Stub handler for unhandled master PIC IRQs (IRQ 2-7) ---
global _irq_stub_master_asm
_irq_stub_master_asm:
    pusha
    mov al, 0x20
    out 0x20, al
    popa
    iret

; --- Stub handler for unhandled slave PIC IRQs (IRQ 8-15 except 12) ---
global _irq_stub_slave_asm
_irq_stub_slave_asm:
    pusha
    mov al, 0x20
    out 0xA0, al
    out 0x20, al
    popa
    iret

; --- CPU Exception Handlers ---

global _exception_gpf_asm
extern _exception_handler_gpf
_exception_gpf_asm:
    pusha
    mov eax, [esp + 32]
    push eax
    call _exception_handler_gpf
    add esp, 4
    popa
    add esp, 4
    iret

global _exception_pf_asm
extern _exception_handler_pf
_exception_pf_asm:
    pusha
    mov eax, [esp + 32]
    push eax
    call _exception_handler_pf
    add esp, 4
    popa
    add esp, 4
    iret

; --- Generic exception stub (for unhandled CPU exceptions) ---
global _exception_stub_asm
_exception_stub_asm:
    iret
