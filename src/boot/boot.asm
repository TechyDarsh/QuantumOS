; ==========================================
; QuantumOS Bootloader (Fixed)
; Boots the system, sets up VESA VBE,
; loads the kernel, switches to PMODE.
; ==========================================

[org 0x7C00]
[bits 16]

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    mov [BOOT_DRIVE], dl

    ; --- 1. Query VESA VBE Info ---
    mov ax, 0x4F00
    xor bx, bx
    mov es, bx
    mov di, 0x8000          ; Put VbeInfoBlock at 0x8000
    int 0x10
    cmp ax, 0x004F
    jne vbe_error

    ; Get video mode list pointer
    mov si, [0x8000 + 14]   ; Offset
    mov ax, [0x8000 + 16]   ; Segment
    mov fs, ax

.find_mode_loop:
    mov cx, [fs:si]
    cmp cx, 0xFFFF
    je vbe_error            ; End of list, no mode found

    ; Query Mode Info for this mode
    push si
    mov ax, 0x4F01
    xor bx, bx
    mov es, bx
    mov di, 0x9000          ; Put ModeInfoBlock at 0x9000
    int 0x10
    pop si

    cmp ax, 0x004F
    jne .next_mode

    ; Check XResolution
    cmp word [0x9000 + 0x12], 800
    jne .next_mode
    ; Check YResolution
    cmp word [0x9000 + 0x14], 600
    jne .next_mode
    ; Check BitsPerPixel
    cmp byte [0x9000 + 0x19], 32
    je .mode_found

.next_mode:
    add si, 2
    jmp .find_mode_loop

.mode_found:
    ; CX contains the mode number!
    ; Save framebuffer parameters to 0x7000
    mov eax, [0x9000 + 0x28] ; PhysBasePtr
    mov dword [0x7000], eax
    mov dword [0x7004], 800
    mov dword [0x7008], 600
    mov dword [0x700C], 32

    ; --- 2. Set VESA VBE Mode with Linear Frame Buffer ---
    mov ax, 0x4F02
    mov bx, cx
    or bx, 0x4000           ; Enable Linear Frame Buffer bit (bit 14)
    int 0x10
    cmp ax, 0x004F
    jne vbe_error

    ; --- 3. Load Kernel from Disk (one sector at a time for reliability) ---
    mov ax, 0x1000
    mov es, ax
    xor bx, bx

    mov word [sectors_left], 320
    mov byte [cur_sect], 2
    mov byte [cur_head], 0
    mov byte [cur_cyl], 0

.read_loop:
    cmp word [sectors_left], 0
    je .read_done

    mov ah, 0x02
    mov al, 1
    mov ch, [cur_cyl]
    mov cl, [cur_sect]
    mov dh, [cur_head]
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc disk_error

    dec word [sectors_left]

    ; Advance ES by 32 paragraphs (512 bytes)
    mov ax, es
    add ax, 32
    mov es, ax

    ; Advance CHS
    inc byte [cur_sect]
    cmp byte [cur_sect], 19
    jl .read_loop
    mov byte [cur_sect], 1
    inc byte [cur_head]
    cmp byte [cur_head], 2
    jl .read_loop
    mov byte [cur_head], 0
    inc byte [cur_cyl]
    jmp .read_loop

.read_done:

    ; --- 4. Switch to 32-bit Protected Mode ---
    cli

    ; Enable A20 Gate
    in al, 0x92
    or al, 0x02
    out 0x92, al

    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp 0x08:pm_entry

vbe_error:
    mov si, MSG_VBE_ERR
    call print16
    jmp halt

disk_error:
    mov si, MSG_DISK_ERR
    call print16
    jmp halt

halt:
    cli
    hlt
    jmp halt

print16:
    pusha
    mov ah, 0x0E
.loop:
    lodsb
    or al, al
    jz .done
    int 0x10
    jmp .loop
.done:
    popa
    ret

; --- GDT ---
gdt_start:
    dd 0, 0
gdt_code:
    dw 0xFFFF, 0x0000
    db 0x00, 0x9A, 0xCF, 0x00
gdt_data:
    dw 0xFFFF, 0x0000
    db 0x00, 0x92, 0xCF, 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; --- Protected Mode Entry ---
[bits 32]
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    mov ebp, esp
    jmp 0x10000

; --- Data ---
BOOT_DRIVE   db 0
sectors_left dw 0
cur_sect     db 0
cur_head     db 0
cur_cyl      db 0
MSG_VBE_ERR  db "VBE Error!", 0
MSG_DISK_ERR db "Disk Error!", 0

times 510-($-$$) db 0
dw 0xAA55
