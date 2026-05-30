# ==========================================
# QuantumOS Makefile
# Builds bootloader, kernel, and creates a
# 1.44MB bootable floppy disk image.
# ==========================================

CC = gcc
LD = ld
NASM = nasm
OBJCOPY = objcopy

CFLAGS = -m32 -march=i386 -ffreestanding -O2 -Wall -Wextra -Wno-unused-parameter -fno-pic -fno-stack-protector -fno-pie -nostdlib -fno-builtin -mno-80387 -mno-sse -mno-mmx -mno-stack-arg-probe -Isrc/kernel
ASMFLAGS = -f win32
LDFLAGS = -m i386pe -T link.ld

# Source and build files
BOOT_SRC = src/boot/boot.asm
KERNEL_ENTRY_SRC = src/kernel/kernel_entry.asm
KERNEL_C_SRC = src/kernel/kernel.c

BOOT_BIN = boot.bin
KERNEL_ENTRY_OBJ = kernel_entry.o
KERNEL_C_OBJ = kernel.o
KERNEL_TMP = kernel.tmp
KERNEL_BIN = kernel.bin
IMG = quantum_os.img

.PHONY: all clean run

all: $(IMG)

$(BOOT_BIN): $(BOOT_SRC)
	$(NASM) -f bin $< -o $@

$(KERNEL_ENTRY_OBJ): $(KERNEL_ENTRY_SRC)
	$(NASM) $(ASMFLAGS) $< -o $@

$(KERNEL_C_OBJ): $(KERNEL_C_SRC) src/kernel/font.h
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL_BIN): $(KERNEL_ENTRY_OBJ) $(KERNEL_C_OBJ)
	$(LD) $(LDFLAGS) -o $(KERNEL_TMP) $(KERNEL_ENTRY_OBJ) $(KERNEL_C_OBJ)
	$(OBJCOPY) -O binary $(KERNEL_TMP) $@

$(IMG): $(BOOT_BIN) $(KERNEL_BIN)
	powershell -Command "[System.IO.File]::WriteAllBytes('$(IMG)', [System.IO.File]::ReadAllBytes('$(BOOT_BIN)') + [System.IO.File]::ReadAllBytes('$(KERNEL_BIN)') + (New-Object Byte[](1474560 - (Get-Item '$(BOOT_BIN)').Length - (Get-Item '$(KERNEL_BIN)').Length)))"

run: $(IMG)
	qemu-system-i386 -drive format=raw,file=$(IMG),if=floppy -m 128M -rtc base=localtime

clean:
	-del /Q $(BOOT_BIN) $(KERNEL_ENTRY_OBJ) $(KERNEL_C_OBJ) $(KERNEL_TMP) $(KERNEL_BIN) $(IMG) 2>nul
