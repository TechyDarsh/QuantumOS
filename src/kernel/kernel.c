#include <stdint.h>
#include <stddef.h>
#include "font.h"
#include "background1.h"



// Low-level port I/O routines (implemented in assembly)
extern uint8_t inb(uint16_t port);
extern void outb(uint16_t port, uint8_t value);
extern uint16_t inw(uint16_t port);
extern void outw(uint16_t port, uint16_t value);
extern uint32_t inl(uint16_t port);
extern void outl(uint16_t port, uint32_t value);
extern void lidt(uint32_t idt_ptr);
extern void enable_interrupts(void);
extern void disable_interrupts(void);

// Assembly ISR entry wrappers
extern void timer_handler_asm(void);
extern void keyboard_handler_asm(void);
extern void mouse_handler_asm(void);
extern void exception_gpf_asm(void);
extern void exception_pf_asm(void);
extern void exception_stub_asm(void);
extern void irq_stub_master_asm(void);
extern void irq_stub_slave_asm(void);

// Memory allocations
#define BACKBUFFER_ADDR 0x200000

// Hardware CMOS RTC Ports
#define CMOS_ADDR_PORT 0x70
#define CMOS_DATA_PORT 0x71

// VBE Info (Written by the bootloader at 0x7000)
#define VBE_FB_PTR       (*(volatile uint32_t*)0x7000)
#define VBE_SCREEN_W     (*(volatile uint32_t*)0x7004)
#define VBE_SCREEN_H     (*(volatile uint32_t*)0x7008)
#define VBE_BPP          (*(volatile uint32_t*)0x700C)

// Shell buffer geometry
#define SHELL_ROWS 18
#define SHELL_COLS 54

#define SCREEN_W 800
#define SCREEN_H 600

// Blinking cursor ticks
volatile uint32_t system_ticks = 0;

// -------------------------------------------------------------------------
// Custom String Library (freestanding) - correct signatures
// -------------------------------------------------------------------------
size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

char* strcpy(char* dest, const char* src) {
    size_t i = 0;
    while ((dest[i] = src[i])) { i++; }
    return dest;
}

char* strcat(char* dest, const char* src) {
    size_t d_len = strlen(dest);
    size_t i = 0;
    while ((dest[d_len + i] = src[i])) { i++; }
    return dest;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    if (n == 0) return 0;
    while (n-- > 0 && *s1 && (*s1 == *s2)) {
        if (n == 0 || *s1 == '\0') break;
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

// CRITICAL: signature must match what GCC internally generates calls to
void* memset(void* dest, int val, size_t count) {
    uint8_t* temp = (uint8_t*)dest;
    uint8_t byte_val = (uint8_t)val;
    for (size_t i = 0; i < count; i++) {
        temp[i] = byte_val;
    }
    return dest;
}

void* memcpy(void* dest, const void* src, size_t count) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < count; i++) {
        d[i] = s[i];
    }
    return dest;
}

void itoa(int val, char* str, int base) {
    int i = 0;
    int is_negative = 0;
    
    if (val == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return;
    }
    
    if (val < 0 && base == 10) {
        is_negative = 1;
        val = -val;
    }
    
    unsigned int uval = (unsigned int)val;

    while (uval > 0) {
        int rem = uval % base;
        str[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
        uval = uval / base;
    }
    
    if (is_negative) {
        str[i++] = '-';
    }
    
    str[i] = '\0';
    
    // Reverse string
    int start_idx = 0;
    int end_idx = i - 1;
    while (start_idx < end_idx) {
        char temp = str[start_idx];
        str[start_idx] = str[end_idx];
        str[end_idx] = temp;
        start_idx++;
        end_idx--;
    }
}

void snprintf(char* buf, size_t max_len, const char* fmt, ...) {
    uint32_t* arg_ptr = (uint32_t*)&fmt + 1;
    size_t idx = 0;
    
    while (*fmt && idx < max_len - 1) {
        if (*fmt == '%') {
            fmt++;
            if (*fmt == '\0') break;
            if (*fmt == 's') {
                const char* s = (const char*)(*arg_ptr++);
                if (!s) s = "(null)";
                while (*s && idx < max_len - 1) {
                    buf[idx++] = *s++;
                }
            } else if (*fmt == 'd') {
                int val = (int)(*arg_ptr++);
                char int_buf[32];
                itoa(val, int_buf, 10);
                char* p = int_buf;
                while (*p && idx < max_len - 1) {
                    buf[idx++] = *p++;
                }
            } else if (*fmt == 'x') {
                uint32_t val = (uint32_t)(*arg_ptr++);
                char hex_buf[32];
                itoa(val, hex_buf, 16);
                char* p = hex_buf;
                while (*p && idx < max_len - 1) {
                    buf[idx++] = *p++;
                }
            } else if (*fmt == 'c') {
                char c = (char)(*arg_ptr++);
                buf[idx++] = c;
            } else if (*fmt == '%') {
                buf[idx++] = '%';
            }
        } else {
            buf[idx++] = *fmt;
        }
        fmt++;
    }
    buf[idx] = '\0';
}

// -------------------------------------------------------------------------
// CMOS RTC Reader
// -------------------------------------------------------------------------
int cmos_update_in_progress(void) {
    outb(CMOS_ADDR_PORT, 0x0A);
    return (inb(CMOS_DATA_PORT) & 0x80);
}

uint8_t get_cmos_register(int reg) {
    outb(CMOS_ADDR_PORT, (uint8_t)reg);
    return inb(CMOS_DATA_PORT);
}

void read_rtc(int* h, int* m, int* s) {
    while (cmos_update_in_progress());
    
    uint8_t sec = get_cmos_register(0x00);
    uint8_t min = get_cmos_register(0x02);
    uint8_t hr = get_cmos_register(0x04);
    uint8_t registerB = get_cmos_register(0x0B);

    if (!(registerB & 4)) {
        sec = ((sec & 0x0F) + ((sec >> 4) * 10));
        min = ((min & 0x0F) + ((min >> 4) * 10));
        hr = ((hr & 0x0F) + ((hr >> 4) * 10));
    }

    *s = sec;
    *m = min;
    *h = hr;
}

// -------------------------------------------------------------------------
// VBE Graphics & Drawing Engine
// -------------------------------------------------------------------------
static inline void putpixel_back(int x, int y, uint32_t color) {
    if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H) {
        ((uint32_t*)BACKBUFFER_ADDR)[y * SCREEN_W + x] = color;
    }
}

void swap_buffer(void) {
    uint32_t* fb = (uint32_t*)VBE_FB_PTR;
    uint32_t* bb = (uint32_t*)BACKBUFFER_ADDR;
    int total = SCREEN_W * SCREEN_H;
    for (int i = 0; i < total; i++) {
        fb[i] = bb[i];
    }
}

void clear_screen_back(uint32_t color) {
    uint32_t* bb = (uint32_t*)BACKBUFFER_ADDR;
    int total = SCREEN_W * SCREEN_H;
    for (int i = 0; i < total; i++) {
        bb[i] = color;
    }
}

void draw_rect_back(int x, int y, int w, int h, uint32_t color) {
    for (int cy = y; cy < y + h; cy++) {
        for (int cx = x; cx < x + w; cx++) {
            putpixel_back(cx, cy, color);
        }
    }
}

void draw_rect_outline_back(int x, int y, int w, int h, uint32_t color, int thickness) {
    draw_rect_back(x, y, w, thickness, color);
    draw_rect_back(x, y + h - thickness, w, thickness, color);
    draw_rect_back(x, y, thickness, h, color);
    draw_rect_back(x + w - thickness, y, thickness, h, color);
}

// FIXED: Integer-only gradient - NO floating point.
// The original used float which crashes without FPU init on i386.
void draw_gradient_vertical_back(int x, int y, int w, int h, uint32_t color1, uint32_t color2) {
    int r1 = (color1 >> 16) & 0xFF;
    int g1 = (color1 >> 8) & 0xFF;
    int b1 = color1 & 0xFF;

    int r2 = (color2 >> 16) & 0xFF;
    int g2 = (color2 >> 8) & 0xFF;
    int b2 = color2 & 0xFF;

    if (h <= 0) return;

    for (int cy = y; cy < y + h; cy++) {
        int t = cy - y;
        int r = r1 + (r2 - r1) * t / h;
        int g = g1 + (g2 - g1) * t / h;
        int b = b1 + (b2 - b1) * t / h;
        uint32_t color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        
        for (int cx = x; cx < x + w; cx++) {
            putpixel_back(cx, cy, color);
        }
    }
}

void draw_image_wallpaper_back(const uint16_t* img_data, int img_w, int img_h) {
    for (int img_y = 0; img_y < img_h; img_y++) {
        int start_y = img_y * 4;
        int row_offset = img_y * img_w;
        for (int img_x = 0; img_x < img_w; img_x++) {
            int start_x = img_x * 4;
            uint16_t color565 = img_data[row_offset + img_x];
            uint32_t r = ((color565 >> 11) & 0x1F) << 3;
            uint32_t g = ((color565 >> 5) & 0x3F) << 2;
            uint32_t b = (color565 & 0x1F) << 3;
            uint32_t color32 = (r << 16) | (g << 8) | b;
            
            for (int dy = 0; dy < 4; dy++) {
                for (int dx = 0; dx < 4; dx++) {
                    putpixel_back(start_x + dx, start_y + dy, color32);
                }
            }
        }
    }
}

// -------------------------------------------------------------------------
// IDT & Interrupt Handling Setup
// -------------------------------------------------------------------------
#pragma pack(push, 1)
struct idt_entry {
    uint16_t base_low;
    uint16_t selector;
    uint8_t zero;
    uint8_t flags;
    uint16_t base_high;
};

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
};
#pragma pack(pop)

struct idt_entry idt[256];
struct idt_ptr idtp;

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low = (base & 0xFFFF);
    idt[num].base_high = ((base >> 16) & 0xFFFF);
    idt[num].selector = sel;
    idt[num].zero = 0;
    idt[num].flags = flags;
}

void pic_remap(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0x00);
    outb(0xA1, 0x00);
}

void idt_install(void) {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base = (uint32_t)&idt;
    
    memset(&idt, 0, sizeof(struct idt_entry) * 256);
    
    pic_remap();
    
    // CPU Exceptions
    idt_set_gate(0x0D, (uint32_t)exception_gpf_asm, 0x08, 0x8E);
    idt_set_gate(0x0E, (uint32_t)exception_pf_asm, 0x08, 0x8E);
    
    // Master PIC IRQs 0-7 -> INT 0x20-0x27
    idt_set_gate(0x20, (uint32_t)timer_handler_asm, 0x08, 0x8E);     // IRQ 0: Timer
    idt_set_gate(0x21, (uint32_t)keyboard_handler_asm, 0x08, 0x8E);  // IRQ 1: Keyboard
    idt_set_gate(0x22, (uint32_t)irq_stub_master_asm, 0x08, 0x8E);   // IRQ 2: Cascade
    idt_set_gate(0x23, (uint32_t)irq_stub_master_asm, 0x08, 0x8E);   // IRQ 3
    idt_set_gate(0x24, (uint32_t)irq_stub_master_asm, 0x08, 0x8E);   // IRQ 4
    idt_set_gate(0x25, (uint32_t)irq_stub_master_asm, 0x08, 0x8E);   // IRQ 5
    idt_set_gate(0x26, (uint32_t)irq_stub_master_asm, 0x08, 0x8E);   // IRQ 6
    idt_set_gate(0x27, (uint32_t)irq_stub_master_asm, 0x08, 0x8E);   // IRQ 7: Spurious
    
    // Slave PIC IRQs 8-15 -> INT 0x28-0x2F
    idt_set_gate(0x28, (uint32_t)irq_stub_slave_asm, 0x08, 0x8E);    // IRQ 8
    idt_set_gate(0x29, (uint32_t)irq_stub_slave_asm, 0x08, 0x8E);    // IRQ 9
    idt_set_gate(0x2A, (uint32_t)irq_stub_slave_asm, 0x08, 0x8E);    // IRQ 10
    idt_set_gate(0x2B, (uint32_t)irq_stub_slave_asm, 0x08, 0x8E);    // IRQ 11
    idt_set_gate(0x2C, (uint32_t)mouse_handler_asm, 0x08, 0x8E);     // IRQ 12: Mouse
    idt_set_gate(0x2D, (uint32_t)irq_stub_slave_asm, 0x08, 0x8E);    // IRQ 13
    idt_set_gate(0x2E, (uint32_t)irq_stub_slave_asm, 0x08, 0x8E);    // IRQ 14
    idt_set_gate(0x2F, (uint32_t)irq_stub_slave_asm, 0x08, 0x8E);    // IRQ 15
    
    lidt((uint32_t)&idtp);
}

// Exception handlers (BSOD)
void exception_handler_gpf(uint32_t err_code) {
    disable_interrupts();
    clear_screen_back(0x000088);
    draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 50, 50, "SYSTEM EXCEPTION: GENERAL PROTECTION FAULT", 0xFFFFFF, 2);
    char buf[64];
    snprintf(buf, sizeof(buf), "Error Code: 0x%x", err_code);
    draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 50, 100, buf, 0xFFFFFF, 1);
    draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 50, 130, "The system has been halted to prevent damage.", 0xFFFFFF, 1);
    draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 50, 160, "Press the power button or close QEMU to restart.", 0xAAAAAA, 1);
    swap_buffer();
    while (1) { __asm__ volatile("hlt"); }
}

void exception_handler_pf(uint32_t err_code) {
    disable_interrupts();
    uint32_t fault_addr;
    __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));
    clear_screen_back(0x000088);
    draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 50, 50, "SYSTEM EXCEPTION: PAGE FAULT", 0xFFFFFF, 2);
    char buf[128];
    snprintf(buf, sizeof(buf), "Address: 0x%x | Error Code: 0x%x", fault_addr, err_code);
    draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 50, 100, buf, 0xFFFFFF, 1);
    draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 50, 130, "The system has been halted to prevent damage.", 0xFFFFFF, 1);
    swap_buffer();
    while (1) { __asm__ volatile("hlt"); }
}

// Timer tick handler
void timer_handler(void) {
    system_ticks++;
}

// -------------------------------------------------------------------------
// Keyboard Driver
// -------------------------------------------------------------------------
volatile int shift_pressed = 0;
volatile int alt_pressed = 0;
volatile int caps_lock = 0;
volatile uint8_t key_states[128] = {0};


static const char kbd_us[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8',
  '9', '0', '-', '=', '\b',
  '\t',
  'q', 'w', 'e', 'r',
  't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,
  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
 '\'', '`',   0,
 '\\', 'z', 'x', 'c', 'v', 'b', 'n',
  'm', ',', '.', '/',   0,
  '*',
    0,
  ' ',
    0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0,
    0, 0, 0,
  '-',
    0, 0, 0,
  '+',
    0, 0, 0, 0, 0
};

static const char kbd_us_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*',
  '(', ')', '_', '+', '\b',
  '\t',
  'Q', 'W', 'E', 'R',
  'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,
  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
 '"', '~',   0,
  '|', 'Z', 'X', 'C', 'V', 'B', 'N',
  'M', '<', '>', '?',   0,
  '*',
    0,
  ' ',
    0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0,
    0, 0, 0,
  '-',
    0, 0, 0,
  '+',
    0, 0, 0, 0, 0
};

// Window definitions and state
#define WINDOW_SHELL 0
#define WINDOW_LETTERPAD 1
#define WINDOW_SYSINFO 2
#define WINDOW_KBDVIS 3
#define WINDOW_GAMESMENU 4
#define WINDOW_CHESS 5

#define NUM_WINDOWS 6

typedef struct {
    int x, y, w, h;
    const char* title;
    int is_visible;
    int is_dragged;
    int is_focused;
    int type;
} Window;

typedef struct {
    int dx, dy, dw, dh;
    uint8_t scancode;
    const char* label;
} VisualKey;

VisualKey visual_keys[60] = {
    // Row 1
    {25, 34, 30, 24, 0x01, "ESC"},
    {57, 34, 26, 24, 0x02, "1"},
    {85, 34, 26, 24, 0x03, "2"},
    {113, 34, 26, 24, 0x04, "3"},
    {141, 34, 26, 24, 0x05, "4"},
    {169, 34, 26, 24, 0x06, "5"},
    {197, 34, 26, 24, 0x07, "6"},
    {225, 34, 26, 24, 0x08, "7"},
    {253, 34, 26, 24, 0x09, "8"},
    {281, 34, 26, 24, 0x0A, "9"},
    {309, 34, 26, 24, 0x0B, "0"},
    {337, 34, 26, 24, 0x0C, "-"},
    {365, 34, 26, 24, 0x0D, "="},
    {393, 34, 62, 24, 0x0E, "BACK"},
    
    // Row 2
    {25, 60, 42, 24, 0x0F, "TAB"},
    {69, 60, 26, 24, 0x10, "Q"},
    {97, 60, 26, 24, 0x11, "W"},
    {125, 60, 26, 24, 0x12, "E"},
    {153, 60, 26, 24, 0x13, "R"},
    {181, 60, 26, 24, 0x14, "T"},
    {209, 60, 26, 24, 0x15, "Y"},
    {237, 60, 26, 24, 0x16, "U"},
    {265, 60, 26, 24, 0x17, "I"},
    {293, 60, 26, 24, 0x18, "O"},
    {321, 60, 26, 24, 0x19, "P"},
    {349, 60, 26, 24, 0x1A, "["},
    {377, 60, 26, 24, 0x1B, "]"},
    {405, 60, 50, 24, 0x1C, "ENT"},
    
    // Row 3
    {25, 86, 52, 24, 0x3A, "CAPS"},
    {79, 86, 26, 24, 0x1E, "A"},
    {107, 86, 26, 24, 0x1F, "S"},
    {135, 86, 26, 24, 0x20, "D"},
    {163, 86, 26, 24, 0x21, "F"},
    {191, 86, 26, 24, 0x22, "G"},
    {219, 86, 26, 24, 0x23, "H"},
    {247, 86, 26, 24, 0x24, "J"},
    {275, 86, 26, 24, 0x25, "K"},
    {303, 86, 26, 24, 0x26, "L"},
    {331, 86, 26, 24, 0x27, ";"},
    {359, 86, 26, 24, 0x28, "'"},
    {387, 86, 68, 24, 0x2B, "\\"},
    
    // Row 4
    {25, 112, 66, 24, 0x2A, "SHIFT"},
    {93, 112, 26, 24, 0x2C, "Z"},
    {121, 112, 26, 24, 0x2D, "X"},
    {149, 112, 26, 24, 0x2E, "C"},
    {177, 112, 26, 24, 0x2F, "V"},
    {205, 112, 26, 24, 0x30, "B"},
    {233, 112, 26, 24, 0x31, "N"},
    {261, 112, 26, 24, 0x32, "M"},
    {289, 112, 26, 24, 0x33, ","},
    {317, 112, 26, 24, 0x34, "."},
    {345, 112, 26, 24, 0x35, "/"},
    {373, 112, 82, 24, 0x36, "SHIFT"},
    
    // Row 5
    {25, 138, 52, 24, 0x1D, "CTRL"},
    {79, 138, 52, 24, 0x38, "ALT"},
    {133, 138, 192, 24, 0x39, "SPACE"},
    {327, 138, 52, 24, 0x38, "ALT"},
    {381, 138, 74, 24, 0x1D, "CTRL"}
};

Window windows[NUM_WINDOWS] = {
    {50, 50, 450, 240, "Quantum Shell", 0, 0, 0, WINDOW_SHELL},
    {250, 100, 420, 280, "LetterPad Text Editor", 0, 0, 0, WINDOW_LETTERPAD},
    {350, 180, 320, 160, "System Info", 0, 0, 0, WINDOW_SYSINFO},
    {160, 180, 480, 200, "Keyboard Visualizer", 0, 0, 0, WINDOW_KBDVIS},
    {100, 100, 400, 240, "Games Menu", 0, 0, 0, WINDOW_GAMESMENU},
    {160, 80, 480, 360, "Chess", 0, 0, 0, WINDOW_CHESS}
};

// -------------------------------------------------------------------------
// Chess Game Constants and Variables
// -------------------------------------------------------------------------
#define CHESS_EMPTY 0
#define CHESS_WHITE 1
#define CHESS_BLACK 2

#define TYPE_PAWN 1
#define TYPE_KNIGHT 2
#define TYPE_BISHOP 3
#define TYPE_ROOK 4
#define TYPE_QUEEN 5
#define TYPE_KING 6

#define CHESS_SIDE(p) ((p) >> 4)
#define CHESS_TYPE(p) ((p) & 0x0F)
#define MAKE_PIECE(side, type) (((side) << 4) | (type))

static const uint8_t initial_chess_board[8][8] = {
    {MAKE_PIECE(CHESS_BLACK, TYPE_ROOK), MAKE_PIECE(CHESS_BLACK, TYPE_KNIGHT), MAKE_PIECE(CHESS_BLACK, TYPE_BISHOP), MAKE_PIECE(CHESS_BLACK, TYPE_QUEEN), MAKE_PIECE(CHESS_BLACK, TYPE_KING), MAKE_PIECE(CHESS_BLACK, TYPE_BISHOP), MAKE_PIECE(CHESS_BLACK, TYPE_KNIGHT), MAKE_PIECE(CHESS_BLACK, TYPE_ROOK)},
    {MAKE_PIECE(CHESS_BLACK, TYPE_PAWN), MAKE_PIECE(CHESS_BLACK, TYPE_PAWN),   MAKE_PIECE(CHESS_BLACK, TYPE_PAWN),   MAKE_PIECE(CHESS_BLACK, TYPE_PAWN),  MAKE_PIECE(CHESS_BLACK, TYPE_PAWN), MAKE_PIECE(CHESS_BLACK, TYPE_PAWN),   MAKE_PIECE(CHESS_BLACK, TYPE_PAWN),   MAKE_PIECE(CHESS_BLACK, TYPE_PAWN)},
    {0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0},
    {0, 0, 0, 0, 0, 0, 0, 0},
    {MAKE_PIECE(CHESS_WHITE, TYPE_PAWN), MAKE_PIECE(CHESS_WHITE, TYPE_PAWN),   MAKE_PIECE(CHESS_WHITE, TYPE_PAWN),   MAKE_PIECE(CHESS_WHITE, TYPE_PAWN),  MAKE_PIECE(CHESS_WHITE, TYPE_PAWN), MAKE_PIECE(CHESS_WHITE, TYPE_PAWN),   MAKE_PIECE(CHESS_WHITE, TYPE_PAWN),   MAKE_PIECE(CHESS_WHITE, TYPE_PAWN)},
    {MAKE_PIECE(CHESS_WHITE, TYPE_ROOK), MAKE_PIECE(CHESS_WHITE, TYPE_KNIGHT), MAKE_PIECE(CHESS_WHITE, TYPE_BISHOP), MAKE_PIECE(CHESS_WHITE, TYPE_QUEEN), MAKE_PIECE(CHESS_WHITE, TYPE_KING), MAKE_PIECE(CHESS_WHITE, TYPE_BISHOP), MAKE_PIECE(CHESS_WHITE, TYPE_KNIGHT), MAKE_PIECE(CHESS_WHITE, TYPE_ROOK)}
};

uint8_t chess_board[8][8];
int chess_active_side = CHESS_WHITE;
int chess_sel_x = -1;
int chess_sel_y = -1;
int chess_mode = 0;   // 0 = none/menu, 1 = 2 Player, 2 = Easy AI, 3 = Med AI, 4 = Hard AI
int chess_status = 0; // 0 = active, 1 = white won, 2 = black won
char chess_msg[64];

typedef struct {
    uint8_t x1, y1, x2, y2;
    int score;
} ChessMove;

void init_chess_game(int mode) {
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            chess_board[y][x] = initial_chess_board[y][x];
        }
    }
    chess_active_side = CHESS_WHITE;
    chess_sel_x = -1;
    chess_sel_y = -1;
    chess_mode = mode;
    chess_status = 0;
    strcpy(chess_msg, "White's turn. Select a piece.");
}

void map_chess_grid(int sx, int sy, int* bx, int* by) {
    if (chess_mode == 1 && chess_active_side == CHESS_BLACK) {
        *bx = 7 - sx;
        *by = 7 - sy;
    } else {
        *bx = sx;
        *by = sy;
    }
}

int is_chess_move_legal(int x1, int y1, int x2, int y2) {
    if (x1 < 0 || x1 >= 8 || y1 < 0 || y1 >= 8) return 0;
    if (x2 < 0 || x2 >= 8 || y2 < 0 || y2 >= 8) return 0;
    if (x1 == x2 && y1 == y2) return 0;
    
    uint8_t p = chess_board[y1][x1];
    if (p == 0) return 0;
    
    int p_side = CHESS_SIDE(p);
    int p_type = CHESS_TYPE(p);
    
    uint8_t target = chess_board[y2][x2];
    if (target != 0 && CHESS_SIDE(target) == p_side) return 0;
    
    int dx = x2 - x1;
    int dy = y2 - y1;
    int abs_dx = dx < 0 ? -dx : dx;
    int abs_dy = dy < 0 ? -dy : dy;
    
    switch (p_type) {
        case TYPE_PAWN: {
            if (p_side == CHESS_WHITE) {
                if (dx == 0 && dy == -1 && target == 0) return 1;
                if (dx == 0 && dy == -2 && y1 == 6 && target == 0 && chess_board[5][x1] == 0) return 1;
                if (abs_dx == 1 && dy == -1 && target != 0 && CHESS_SIDE(target) == CHESS_BLACK) return 1;
            } else {
                if (dx == 0 && dy == 1 && target == 0) return 1;
                if (dx == 0 && dy == 2 && y1 == 1 && target == 0 && chess_board[2][x1] == 0) return 1;
                if (abs_dx == 1 && dy == 1 && target != 0 && CHESS_SIDE(target) == CHESS_WHITE) return 1;
            }
            return 0;
        }
        case TYPE_KNIGHT: {
            if ((abs_dx == 1 && abs_dy == 2) || (abs_dx == 2 && abs_dy == 1)) return 1;
            return 0;
        }
        case TYPE_BISHOP: {
            if (abs_dx != abs_dy) return 0;
            int step_x = dx > 0 ? 1 : -1;
            int step_y = dy > 0 ? 1 : -1;
            int cx = x1 + step_x;
            int cy = y1 + step_y;
            while (cx != x2 && cy != y2) {
                if (chess_board[cy][cx] != 0) return 0;
                cx += step_x;
                cy += step_y;
            }
            return 1;
        }
        case TYPE_ROOK: {
            if (dx != 0 && dy != 0) return 0;
            int step_x = dx == 0 ? 0 : (dx > 0 ? 1 : -1);
            int step_y = dy == 0 ? 0 : (dy > 0 ? 1 : -1);
            int cx = x1 + step_x;
            int cy = y1 + step_y;
            while (cx != x2 || cy != y2) {
                if (chess_board[cy][cx] != 0) return 0;
                cx += step_x;
                cy += step_y;
            }
            return 1;
        }
        case TYPE_QUEEN: {
            if (dx == 0 || dy == 0) {
                int step_x = dx == 0 ? 0 : (dx > 0 ? 1 : -1);
                int step_y = dy == 0 ? 0 : (dy > 0 ? 1 : -1);
                int cx = x1 + step_x;
                int cy = y1 + step_y;
                while (cx != x2 || cy != y2) {
                    if (chess_board[cy][cx] != 0) return 0;
                    cx += step_x;
                    cy += step_y;
                }
                return 1;
            } else if (abs_dx == abs_dy) {
                int step_x = dx > 0 ? 1 : -1;
                int step_y = dy > 0 ? 1 : -1;
                int cx = x1 + step_x;
                int cy = y1 + step_y;
                while (cx != x2 && cy != y2) {
                    if (chess_board[cy][cx] != 0) return 0;
                    cx += step_x;
                    cy += step_y;
                }
                return 1;
            }
            return 0;
        }
        case TYPE_KING: {
            if (abs_dx <= 1 && abs_dy <= 1) return 1;
            return 0;
        }
    }
    return 0;
}

int get_all_legal_moves(int side, ChessMove* moves) {
    int count = 0;
    for (int y1 = 0; y1 < 8; y1++) {
        for (int x1 = 0; x1 < 8; x1++) {
            uint8_t p = chess_board[y1][x1];
            if (p != 0 && CHESS_SIDE(p) == side) {
                for (int y2 = 0; y2 < 8; y2++) {
                    for (int x2 = 0; x2 < 8; x2++) {
                        if (is_chess_move_legal(x1, y1, x2, y2)) {
                            if (count < 256) {
                                moves[count].x1 = x1;
                                moves[count].y1 = y1;
                                moves[count].x2 = x2;
                                moves[count].y2 = y2;
                                moves[count].score = 0;
                                count++;
                            }
                        }
                    }
                }
            }
        }
    }
    return count;
}

int evaluate_board_for_black(void) {
    int score = 0;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            uint8_t p = chess_board[y][x];
            if (p != 0) {
                int val = 0;
                switch (CHESS_TYPE(p)) {
                    case TYPE_PAWN: val = 10; break;
                    case TYPE_KNIGHT: val = 30; break;
                    case TYPE_BISHOP: val = 30; break;
                    case TYPE_ROOK: val = 50; break;
                    case TYPE_QUEEN: val = 90; break;
                    case TYPE_KING: val = 9999; break;
                }
                if (x >= 3 && x <= 4 && y >= 3 && y <= 4) {
                    val += 2;
                }
                if (CHESS_SIDE(p) == CHESS_BLACK) {
                    score += val;
                } else {
                    score -= val;
                }
            }
        }
    }
    return score;
}

void execute_ai_move(void) {
    ChessMove moves[256];
    int count = get_all_legal_moves(CHESS_BLACK, moves);
    if (count == 0) {
        chess_status = 1;
        strcpy(chess_msg, "GAME OVER. White wins!");
        return;
    }
    
    int best_move_idx = 0;
    
    if (chess_mode == 2) {
        best_move_idx = system_ticks % count;
    } else if (chess_mode == 3) {
        int best_score = -999999;
        for (int i = 0; i < count; i++) {
            uint8_t target = chess_board[moves[i].y2][moves[i].x2];
            int score = 0;
            if (target != 0) {
                switch (CHESS_TYPE(target)) {
                    case TYPE_PAWN: score = 10; break;
                    case TYPE_KNIGHT: score = 30; break;
                    case TYPE_BISHOP: score = 30; break;
                    case TYPE_ROOK: score = 50; break;
                    case TYPE_QUEEN: score = 90; break;
                    case TYPE_KING: score = 9999; break;
                }
            }
            score += (system_ticks + i) % 5;
            if (score > best_score) {
                best_score = score;
                best_move_idx = i;
            }
        }
    } else {
        int best_score = -9999999;
        for (int i = 0; i < count; i++) {
            uint8_t temp_src = chess_board[moves[i].y1][moves[i].x1];
            uint8_t temp_dst = chess_board[moves[i].y2][moves[i].x2];
            chess_board[moves[i].y2][moves[i].x2] = temp_src;
            chess_board[moves[i].y1][moves[i].x1] = 0;
            
            ChessMove response_moves[256];
            int white_count = get_all_legal_moves(CHESS_WHITE, response_moves);
            int worst_score = 9999999;
            
            if (white_count == 0) {
                worst_score = 999999;
            } else {
                for (int j = 0; j < white_count; j++) {
                    uint8_t w_temp_src = chess_board[response_moves[j].y1][response_moves[j].x1];
                    uint8_t w_temp_dst = chess_board[response_moves[j].y2][response_moves[j].x2];
                    chess_board[response_moves[j].y2][response_moves[j].x2] = w_temp_src;
                    chess_board[response_moves[j].y1][response_moves[j].x1] = 0;
                    
                    int score = evaluate_board_for_black();
                    if (score < worst_score) {
                        worst_score = score;
                    }
                    
                    chess_board[response_moves[j].y1][response_moves[j].x1] = w_temp_src;
                    chess_board[response_moves[j].y2][response_moves[j].x2] = w_temp_dst;
                }
            }
            
            chess_board[moves[i].y1][moves[i].x1] = temp_src;
            chess_board[moves[i].y2][moves[i].x2] = temp_dst;
            
            if (worst_score > best_score) {
                best_score = worst_score;
                best_move_idx = i;
            }
        }
    }
    
    int x1 = moves[best_move_idx].x1;
    int y1 = moves[best_move_idx].y1;
    int x2 = moves[best_move_idx].x2;
    int y2 = moves[best_move_idx].y2;
    
    uint8_t captured = chess_board[y2][x2];
    chess_board[y2][x2] = chess_board[y1][x1];
    chess_board[y1][x1] = 0;
    
    if (CHESS_TYPE(captured) == TYPE_KING) {
        chess_status = 2;
        strcpy(chess_msg, "White's turn.");
    }
}

static const uint16_t chess_pawn_bmp[16] = {
    0x0000, 0x0000, 0x0180, 0x03C0, 0x03C0, 0x0180, 0x0180, 0x03C0,
    0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x0FF0, 0x1FF8, 0x0000, 0x0000
};
static const uint16_t chess_knight_bmp[16] = {
    0x0000, 0x0180, 0x0380, 0x07C0, 0x0DC0, 0x1FE0, 0x3FE0, 0x3E60,
    0x3C60, 0x1860, 0x18E0, 0x1FE0, 0x0FC0, 0x1FE0, 0x3FF0, 0x0000
};
static const uint16_t chess_bishop_bmp[16] = {
    0x0180, 0x03C0, 0x0180, 0x03C0, 0x07E0, 0x0FF0, 0x0E70, 0x1C78,
    0x1FF8, 0x0FF0, 0x07E0, 0x07E0, 0x0FF0, 0x1FF8, 0x3FFC, 0x0000
};
static const uint16_t chess_rook_bmp[16] = {
    0x0000, 0x36D8, 0x36D8, 0x3FF8, 0x1FF0, 0x0FE0, 0x0FE0, 0x0FE0,
    0x0FE0, 0x0FE0, 0x0FE0, 0x0FE0, 0x1FF0, 0x3FF8, 0x3FFC, 0x0000
};
static const uint16_t chess_queen_bmp[16] = {
    0x0000, 0x1110, 0x3BA8, 0x3BA8, 0x1FF0, 0x1FF0, 0x0FE0, 0x0FE0,
    0x07C0, 0x0FE0, 0x1FF0, 0x3FF8, 0x3FF8, 0x1FF0, 0x3FFC, 0x0000
};
static const uint16_t chess_king_bmp[16] = {
    0x0180, 0x03C0, 0x0180, 0x3BA8, 0x3FF8, 0x1FF0, 0x0FE0, 0x0FE0,
    0x0FE0, 0x0FE0, 0x1FF0, 0x3FF8, 0x3FF8, 0x1FF0, 0x3FFC, 0x0000
};

void draw_chess_piece_icon(int x, int y, int type, int side) {
    const uint16_t* bitmap = 0;
    switch (type) {
        case TYPE_PAWN:   bitmap = chess_pawn_bmp;   break;
        case TYPE_KNIGHT: bitmap = chess_knight_bmp; break;
        case TYPE_BISHOP: bitmap = chess_bishop_bmp; break;
        case TYPE_ROOK:   bitmap = chess_rook_bmp;   break;
        case TYPE_QUEEN:  bitmap = chess_queen_bmp;  break;
        case TYPE_KING:   bitmap = chess_king_bmp;   break;
    }
    if (!bitmap) return;
    
    uint32_t body_color = (side == CHESS_WHITE) ? 0xFFD700 : 0x050505;
    uint32_t shadow_color = (side == CHESS_WHITE) ? 0x332200 : 0xFFFFFF;
    
    for (int dy = 0; dy < 16; dy++) {
        uint16_t row = bitmap[dy];
        for (int dx = 0; dx < 16; dx++) {
            if (row & (1 << (15 - dx))) {
                putpixel_back(x + dx + 1, y + dy + 1, shadow_color);
            }
        }
    }
    for (int dy = 0; dy < 16; dy++) {
        uint16_t row = bitmap[dy];
        for (int dx = 0; dx < 16; dx++) {
            if (row & (1 << (15 - dx))) {
                putpixel_back(x + dx, y + dy, body_color);
            }
        }
    }
}

void handle_chess_click(int bx, int by) {
    if (chess_status != 0) return;
    
    uint8_t p = chess_board[by][bx];
    
    if (chess_sel_x == -1 && chess_sel_y == -1) {
        if (p != 0 && CHESS_SIDE(p) == chess_active_side) {
            chess_sel_x = bx;
            chess_sel_y = by;
            if (chess_active_side == CHESS_WHITE) {
                strcpy(chess_msg, "White piece selected. Click dest.");
            } else {
                strcpy(chess_msg, "Black piece selected. Click dest.");
            }
        }
    } else {
        if (bx == chess_sel_x && by == chess_sel_y) {
            // Deselect
            chess_sel_x = -1;
            chess_sel_y = -1;
            if (chess_active_side == CHESS_WHITE) {
                strcpy(chess_msg, "White's turn.");
            } else {
                strcpy(chess_msg, "Black's turn.");
            }
        } else if (is_chess_move_legal(chess_sel_x, chess_sel_y, bx, by)) {
            uint8_t target = chess_board[by][bx];
            chess_board[by][bx] = chess_board[chess_sel_y][chess_sel_x];
            chess_board[chess_sel_y][chess_sel_x] = 0;
            
            // Auto-promote Pawn to Queen
            if (CHESS_TYPE(chess_board[by][bx]) == TYPE_PAWN && (by == 0 || by == 7)) {
                chess_board[by][bx] = MAKE_PIECE(chess_active_side, TYPE_QUEEN);
            }
            
            if (CHESS_TYPE(target) == TYPE_KING) {
                if (CHESS_SIDE(target) == CHESS_WHITE) {
                    chess_status = 2; // Black won
                    strcpy(chess_msg, "GAME OVER. Black wins!");
                } else {
                    chess_status = 1; // White won
                    strcpy(chess_msg, "GAME OVER. White wins!");
                }
                chess_sel_x = -1;
                chess_sel_y = -1;
            } else {
                chess_sel_x = -1;
                chess_sel_y = -1;
                
                // Toggle turn
                chess_active_side = (chess_active_side == CHESS_WHITE) ? CHESS_BLACK : CHESS_WHITE;
                
                if (chess_active_side == CHESS_BLACK && chess_mode >= 2) {
                    strcpy(chess_msg, "Computer is thinking...");
                    execute_ai_move();
                } else {
                    if (chess_active_side == CHESS_WHITE) {
                        strcpy(chess_msg, "White's turn.");
                    } else {
                        strcpy(chess_msg, "Black's turn.");
                    }
                }
            }
        } else {
            // Illegal move - check if clicked another of our own pieces to switch selection
            if (p != 0 && CHESS_SIDE(p) == chess_active_side) {
                chess_sel_x = bx;
                chess_sel_y = by;
                strcpy(chess_msg, "Selected new piece.");
            } else {
                strcpy(chess_msg, "Illegal move! Try again.");
            }
        }
    }
}

int drag_offset_x = 0;
int drag_offset_y = 0;
int active_win_idx = -1;

#pragma pack(push, 1)
typedef struct {
    char name[16];
    char content[512];
    int size;
    int is_used;
} VirtualFile;
#pragma pack(pop)

#define MAX_VFILES 8
VirtualFile vfs[MAX_VFILES];

void init_vfs(void) {
    memset(vfs, 0, sizeof(vfs));
    
    // File 1: welcome.txt
    strcpy(vfs[0].name, "welcome.txt");
    strcpy(vfs[0].content, "Welcome to QuantumOS!\n\nThis is a simulated in-memory filesystem (VFS).\nYou can use the following commands in the shell:\n  ls         - List all files\n  cat <file> - View file contents\n  write <file> <text> - Write to file\n  rm <file>  - Delete file\n  edit <file> - Edit file in LetterPad\n  run <file> - Run commands from file");
    vfs[0].size = strlen(vfs[0].content);
    vfs[0].is_used = 1;
    
    // File 2: notes.txt
    strcpy(vfs[1].name, "notes.txt");
    strcpy(vfs[1].content, "- Build graphical OS from scratch: DONE\n- Fix IDT structure alignment bug: DONE\n- Implement dynamic VBE resolution scanning: DONE\n- Implement smooth double-buffered rendering: DONE\n- Add interactive terminal & notepad: DONE");
    vfs[1].size = strlen(vfs[1].content);
    vfs[1].is_used = 1;

    // File 3: script.txt
    strcpy(vfs[2].name, "script.txt");
    strcpy(vfs[2].content, "clear\nabout\nsysinfo\nls");
    vfs[2].size = strlen(vfs[2].content);
    vfs[2].is_used = 1;
}

// Application Buffers
char shell_lines[SHELL_ROWS][SHELL_COLS];
int shell_line_count = 0;
char shell_input[64];
int shell_input_idx = 0;

char letterpad_text[2048];
int letterpad_len = 0;

// Graphical Options
int current_wallpaper_style = 4;
int matrix_mode = 0;
int matrix_drops[50];
int start_menu_open = 0;

// Forward declarations
void shell_print(const char* str);
void run_shell_command(const char* cmd);
void change_wallpaper(void);
void reboot_system(void);
void shutdown_system(void);

void focus_window(int idx) {
    for (int i = 0; i < NUM_WINDOWS; i++) {
        windows[i].is_focused = (i == idx);
    }
    active_win_idx = idx;
}

void toggle_window(int idx) {
    windows[idx].is_visible = !windows[idx].is_visible;
    if (windows[idx].is_visible) {
        focus_window(idx);
    }
}

void keyboard_handler(void) {
    uint8_t scancode = inb(0x60);
    
    // Track key states (pressed / released)
    if (scancode & 0x80) {
        uint8_t rel_scancode = scancode & 0x7F;
        if (rel_scancode < 128) {
            key_states[rel_scancode] = 0;
        }
    } else {
        if (scancode < 128) {
            key_states[scancode] = 1;
        }
    }
    
    if (scancode == 0x2A || scancode == 0x36) { shift_pressed = 1; return; }
    if (scancode == 0xAA || scancode == 0xB6) { shift_pressed = 0; return; }
    if (scancode == 0x38) { alt_pressed = 1; return; }
    if (scancode == 0xB8) { alt_pressed = 0; return; }
    if (scancode == 0x3A) { caps_lock = !caps_lock; return; }
    
    // Ignore key releases
    if (scancode & 0x80) return;
    
    // Matrix mode: any key exits
    if (matrix_mode) {
        matrix_mode = 0;
        memset(shell_lines, 0, sizeof(shell_lines));
        shell_line_count = 0;
        shell_print("Exited matrix digital rain.");
        shell_print("");
        return;
    }

    // Alt Shortcut Keys
    if (alt_pressed) {
        if (scancode == 0x1F) toggle_window(WINDOW_SHELL);
        else if (scancode == 0x26) toggle_window(WINDOW_LETTERPAD);
        else if (scancode == 0x17) toggle_window(WINDOW_SYSINFO);
        else if (scancode == 0x25) toggle_window(WINDOW_KBDVIS);  // Alt + K
        else if (scancode == 0x22) toggle_window(WINDOW_GAMESMENU); // Alt + G
        else if (scancode == 0x2E) {
            if (active_win_idx >= 0 && windows[active_win_idx].is_visible)
                windows[active_win_idx].is_visible = 0;
        }
        else if (scancode == 0x13) reboot_system();
        else if (scancode == 0x19) shutdown_system();
        else if (scancode == 0x30) change_wallpaper();
        return;
    }
    
    // Text input for focused window
    if (active_win_idx == WINDOW_SHELL && windows[WINDOW_SHELL].is_visible) {
        char ascii = shift_pressed ? kbd_us_shift[scancode] : kbd_us[scancode];
        if (caps_lock && ascii >= 'a' && ascii <= 'z') ascii -= 32;
        else if (caps_lock && ascii >= 'A' && ascii <= 'Z') ascii += 32;

        if (ascii) {
            if (ascii == '\n') {
                shell_input[shell_input_idx] = '\0';
                char prompt_echo[128];
                snprintf(prompt_echo, sizeof(prompt_echo), "quantum_os> %s", shell_input);
                shell_print(prompt_echo);
                run_shell_command(shell_input);
                shell_input_idx = 0;
                shell_input[0] = '\0';
            } else if (ascii == '\b') {
                if (shell_input_idx > 0) {
                    shell_input_idx--;
                    shell_input[shell_input_idx] = '\0';
                }
            } else {
                if (shell_input_idx < (int)sizeof(shell_input) - 2) {
                    shell_input[shell_input_idx++] = ascii;
                    shell_input[shell_input_idx] = '\0';
                }
            }
        }
    } else if (active_win_idx == WINDOW_LETTERPAD && windows[WINDOW_LETTERPAD].is_visible) {
        char ascii = shift_pressed ? kbd_us_shift[scancode] : kbd_us[scancode];
        if (caps_lock && ascii >= 'a' && ascii <= 'z') ascii -= 32;
        else if (caps_lock && ascii >= 'A' && ascii <= 'Z') ascii += 32;

        if (ascii) {
            if (ascii == '\b') {
                if (letterpad_len > 0) {
                    letterpad_len--;
                    letterpad_text[letterpad_len] = '\0';
                }
            } else {
                if (letterpad_len < (int)sizeof(letterpad_text) - 2) {
                    letterpad_text[letterpad_len++] = ascii;
                    letterpad_text[letterpad_len] = '\0';
                }
            }
        }
    }
}

// -------------------------------------------------------------------------
// Mouse Driver
// -------------------------------------------------------------------------
volatile int mouse_x = 400;
volatile int mouse_y = 300;
volatile int mouse_left_pressed = 0;
volatile int mouse_right_pressed = 0;
int prev_mouse_left_pressed = 0;

volatile int mouse_cycle = 0;
volatile uint8_t mouse_packet[3];

void mouse_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) {
        while (timeout-- && !(inb(0x64) & 1));
    } else {
        while (timeout-- && (inb(0x64) & 2));
    }
}

void mouse_write(uint8_t data) {
    mouse_wait(1);
    outb(0x64, 0xD4);
    mouse_wait(1);
    outb(0x60, data);
}

uint8_t mouse_read(void) {
    mouse_wait(0);
    return inb(0x60);
}

void mouse_install(void) {
    uint8_t status;
    
    mouse_wait(1);
    outb(0x64, 0xA8);
    
    mouse_wait(1);
    outb(0x64, 0x20);
    mouse_wait(0);
    status = inb(0x60);
    
    // Enable interrupts for both KBD & Mouse (bits 0,1)
    // Enable clock lines for both KBD & Mouse (clear bits 4,5)
    status = (status | 3) & ~0x30;
    
    mouse_wait(1);
    outb(0x64, 0x60);
    mouse_wait(1);
    outb(0x60, status);
    
    mouse_write(0xF6);
    mouse_read();
    
    mouse_write(0xF4);
    mouse_read();
}

void mouse_handler(void) {
    uint8_t status = inb(0x64);
    if (!(status & 0x20)) {
        inb(0x60);
        return;
    }
    
    uint8_t data = inb(0x60);
    
    // Resynchronization: first byte must have bit 3 (0x08) set.
    if (mouse_cycle == 0 && !(data & 0x08)) {
        return; // Discard unaligned packet byte
    }
    
    mouse_packet[mouse_cycle] = data;
    mouse_cycle++;
    
    if (mouse_cycle >= 3) {
        mouse_cycle = 0;
        
        int dx = (int)mouse_packet[1];
        int dy = (int)mouse_packet[2];
        
        if (mouse_packet[0] & 0x10) dx |= 0xFFFFFF00;
        if (mouse_packet[0] & 0x20) dy |= 0xFFFFFF00;
        
        mouse_x += dx;
        mouse_y -= dy;
        
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_x >= SCREEN_W) mouse_x = SCREEN_W - 1;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_y >= SCREEN_H) mouse_y = SCREEN_H - 1;
        
        mouse_left_pressed = (mouse_packet[0] & 1);
        mouse_right_pressed = (mouse_packet[0] & 2);
    }
}

// -------------------------------------------------------------------------
// Shell Commands
// -------------------------------------------------------------------------
void shell_print(const char* str) {
    char temp_line[SHELL_COLS];
    int tl_idx = 0;
    
    while (*str) {
        if (*str == '\n' || tl_idx == SHELL_COLS - 1) {
            temp_line[tl_idx] = '\0';
            
            if (shell_line_count >= SHELL_ROWS) {
                for (int j = 0; j < SHELL_ROWS - 1; j++) {
                    strcpy(shell_lines[j], shell_lines[j+1]);
                }
                memset(shell_lines[SHELL_ROWS - 1], 0, SHELL_COLS);
                shell_line_count = SHELL_ROWS - 1;
            }
            strcpy(shell_lines[shell_line_count], temp_line);
            shell_line_count++;
            tl_idx = 0;
            
            if (*str == '\n') { str++; continue; }
        }
        temp_line[tl_idx++] = *str++;
    }
    
    if (tl_idx > 0) {
        temp_line[tl_idx] = '\0';
        if (shell_line_count >= SHELL_ROWS) {
            for (int j = 0; j < SHELL_ROWS - 1; j++) {
                strcpy(shell_lines[j], shell_lines[j+1]);
            }
            memset(shell_lines[SHELL_ROWS - 1], 0, SHELL_COLS);
            shell_line_count = SHELL_ROWS - 1;
        }
        strcpy(shell_lines[shell_line_count], temp_line);
        shell_line_count++;
    }
}

void run_calc_command(const char* args) {
    while (*args == ' ') args++;
    int num1 = 0, sign1 = 1;
    if (*args == '-') { sign1 = -1; args++; }
    while (*args >= '0' && *args <= '9') {
        num1 = num1 * 10 + (*args - '0');
        args++;
    }
    num1 *= sign1;
    
    while (*args == ' ') args++;
    char op = *args;
    if (op == '\0') {
        shell_print("Usage: calc <n1> <op> <n2>");
        return;
    }
    args++;
    
    while (*args == ' ') args++;
    int num2 = 0, sign2 = 1;
    if (*args == '-') { sign2 = -1; args++; }
    if (*args < '0' || *args > '9') {
        shell_print("Error: Missing second operand.");
        return;
    }
    while (*args >= '0' && *args <= '9') {
        num2 = num2 * 10 + (*args - '0');
        args++;
    }
    num2 *= sign2;
    
    int res = 0;
    char buf[64];
    if (op == '+') {
        res = num1 + num2;
        snprintf(buf, sizeof(buf), "%d + %d = %d", num1, num2, res);
    } else if (op == '-') {
        res = num1 - num2;
        snprintf(buf, sizeof(buf), "%d - %d = %d", num1, num2, res);
    } else if (op == '*') {
        res = num1 * num2;
        snprintf(buf, sizeof(buf), "%d * %d = %d", num1, num2, res);
    } else if (op == '/') {
        if (num2 == 0) { shell_print("Error: Division by zero!"); return; }
        res = num1 / num2;
        int rem = num1 % num2;
        if (rem != 0) {
            snprintf(buf, sizeof(buf), "%d / %d = %d rem %d", num1, num2, res, rem);
        } else {
            snprintf(buf, sizeof(buf), "%d / %d = %d", num1, num2, res);
        }
    } else {
        shell_print("Supported operators: + - * /");
        return;
    }
    shell_print(buf);
}

void run_shell_command(const char* cmd) {
    while (*cmd == ' ') cmd++;
    if (strlen(cmd) == 0) return;
    
    if (strcmp(cmd, "help") == 0) {
        shell_print("--- QuantumOS Commands ---");
        shell_print("  help        Show this help");
        shell_print("  about       OS information");
        shell_print("  clear       Clear console");
        shell_print("  sysinfo     System statistics");
        shell_print("  calc N op N Calculator");
        shell_print("  matrix      Matrix rain effect");
        shell_print("  time        System uptime");
        shell_print("  reboot      Restart system");
        shell_print("  poweroff    Shut down");
        shell_print("--- File & App Commands ---");
        shell_print("  open <app>  Open: shell, letterpad, games, chess, kbdvis, sysinfo");
        shell_print("  close <app> Close specified app");
        shell_print("  ls          List files in VFS");
        shell_print("  cat <file>  Print file contents");
        shell_print("  write <file> <text>  Write text (use \\n for newline)");
        shell_print("  rm <file>   Delete file");
        shell_print("  edit <file> Edit file in LetterPad");
        shell_print("  run <file>  Run shell script file");
        shell_print("  theme <0-4> Change wallpaper gradient / image");
    } else if (strcmp(cmd, "about") == 0) {
        shell_print("QuantumOS v1.0");
        shell_print("AI Prototype Operating System");
        shell_print("Written from scratch in C & ASM");
        shell_print("800x600 VBE Linear Framebuffer");
        shell_print("Fully standalone - no host OS deps");
    } else if (strcmp(cmd, "clear") == 0) {
        memset(shell_lines, 0, sizeof(shell_lines));
        shell_line_count = 0;
    } else if (strcmp(cmd, "sysinfo") == 0) {
        shell_print("-- System Information --");
        shell_print("  CPU: x86 32-bit Protected Mode");
        char res_buf[64];
        snprintf(res_buf, sizeof(res_buf), "  Display: %dx%d @ %dbpp VBE", SCREEN_W, SCREEN_H, 32);
        shell_print(res_buf);
        shell_print("  RAM: 128 MB allocated");
        shell_print("  Graphics: Double-Buffered LFB");
        shell_print("  Input: PS/2 Keyboard + Mouse");
    } else if (strncmp(cmd, "calc", 4) == 0) {
        run_calc_command(cmd + 4);
    } else if (strcmp(cmd, "time") == 0) {
        char time_buf[64];
        snprintf(time_buf, sizeof(time_buf), "Uptime: %d seconds", system_ticks / 18);
        shell_print(time_buf);
    } else if (strcmp(cmd, "reboot") == 0) {
        reboot_system();
    } else if (strcmp(cmd, "poweroff") == 0) {
        shutdown_system();
    } else if (strcmp(cmd, "matrix") == 0) {
        matrix_mode = 1;
        for (int i = 0; i < 38; i++) {
            matrix_drops[i] = (system_ticks * (i + 1)) % 15;
        }
    } else if (strncmp(cmd, "open ", 5) == 0) {
        const char* app = cmd + 5;
        while (*app == ' ') app++;
        if (strcmp(app, "shell") == 0 || strcmp(app, "terminal") == 0) {
            windows[WINDOW_SHELL].is_visible = 1;
            focus_window(WINDOW_SHELL);
            shell_print("Opened Quantum Shell.");
        } else if (strcmp(app, "letterpad") == 0 || strcmp(app, "notepad") == 0) {
            windows[WINDOW_LETTERPAD].is_visible = 1;
            focus_window(WINDOW_LETTERPAD);
            shell_print("Opened LetterPad.");
        } else if (strcmp(app, "sysinfo") == 0 || strcmp(app, "info") == 0) {
            windows[WINDOW_SYSINFO].is_visible = 1;
            focus_window(WINDOW_SYSINFO);
            shell_print("Opened System Info.");
        } else if (strcmp(app, "kbdvis") == 0 || strcmp(app, "visualizer") == 0) {
            windows[WINDOW_KBDVIS].is_visible = 1;
            focus_window(WINDOW_KBDVIS);
            shell_print("Opened Keyboard Visualizer.");
        } else if (strcmp(app, "games") == 0 || strcmp(app, "gamesmenu") == 0) {
            windows[WINDOW_GAMESMENU].is_visible = 1;
            focus_window(WINDOW_GAMESMENU);
            shell_print("Opened Games Menu.");
        } else if (strcmp(app, "chess") == 0) {
            windows[WINDOW_CHESS].is_visible = 1;
            focus_window(WINDOW_CHESS);
            shell_print("Opened Chess.");
        } else {
            shell_print("Unknown app. Supported: shell, letterpad, games, chess, kbdvis, sysinfo");
        }
    } else if (strncmp(cmd, "close ", 6) == 0) {
        const char* app = cmd + 6;
        while (*app == ' ') app++;
        if (strcmp(app, "shell") == 0 || strcmp(app, "terminal") == 0) {
            windows[WINDOW_SHELL].is_visible = 0;
            shell_print("Closed Quantum Shell.");
        } else if (strcmp(app, "letterpad") == 0 || strcmp(app, "notepad") == 0) {
            windows[WINDOW_LETTERPAD].is_visible = 0;
            shell_print("Closed LetterPad.");
        } else if (strcmp(app, "sysinfo") == 0 || strcmp(app, "info") == 0) {
            windows[WINDOW_SYSINFO].is_visible = 0;
            shell_print("Closed System Info.");
        } else if (strcmp(app, "kbdvis") == 0 || strcmp(app, "visualizer") == 0) {
            windows[WINDOW_KBDVIS].is_visible = 0;
            shell_print("Closed Keyboard Visualizer.");
        } else if (strcmp(app, "games") == 0 || strcmp(app, "gamesmenu") == 0) {
            windows[WINDOW_GAMESMENU].is_visible = 0;
            shell_print("Closed Games Menu.");
        } else if (strcmp(app, "chess") == 0) {
            windows[WINDOW_CHESS].is_visible = 0;
            shell_print("Closed Chess.");
        } else {
            shell_print("Unknown app. Supported: shell, letterpad, games, chess, kbdvis, sysinfo");
        }
    } else if (strncmp(cmd, "theme ", 6) == 0) {
        const char* arg = cmd + 6;
        while (*arg == ' ') arg++;
        int val = *arg - '0';
        if (val >= 0 && val <= 4) {
            current_wallpaper_style = val;
            char msg[64];
            snprintf(msg, sizeof(msg), "Wallpaper theme changed to %d.", val);
            shell_print(msg);
        } else {
            shell_print("Theme range: 0 to 4.");
        }
    } else if (strcmp(cmd, "ls") == 0) {
        shell_print("--- Virtual Filesystem (VFS) ---");
        int count = 0;
        for (int i = 0; i < MAX_VFILES; i++) {
            if (vfs[i].is_used) {
                char line[64];
                snprintf(line, sizeof(line), "  %s  (%d bytes)", vfs[i].name, vfs[i].size);
                shell_print(line);
                count++;
            }
        }
        if (count == 0) {
            shell_print("  (Empty)");
        }
    } else if (strncmp(cmd, "cat ", 4) == 0) {
        const char* filename = cmd + 4;
        while (*filename == ' ') filename++;
        int found = 0;
        for (int i = 0; i < MAX_VFILES; i++) {
            if (vfs[i].is_used && strcmp(vfs[i].name, filename) == 0) {
                shell_print(vfs[i].content);
                found = 1;
                break;
            }
        }
        if (!found) {
            shell_print("File not found.");
        }
    } else if (strncmp(cmd, "write ", 6) == 0) {
        const char* arg = cmd + 6;
        while (*arg == ' ') arg++;
        char filename[16];
        int fn_idx = 0;
        while (*arg && *arg != ' ' && fn_idx < 15) {
            filename[fn_idx++] = *arg++;
        }
        filename[fn_idx] = '\0';
        
        while (*arg == ' ') arg++;
        
        int slot = -1;
        for (int i = 0; i < MAX_VFILES; i++) {
            if (vfs[i].is_used && strcmp(vfs[i].name, filename) == 0) {
                slot = i;
                break;
            }
        }
        if (slot == -1) {
            for (int i = 0; i < MAX_VFILES; i++) {
                if (!vfs[i].is_used) {
                    slot = i;
                    break;
                }
            }
        }
        
        if (slot != -1) {
            strcpy(vfs[slot].name, filename);
            int c_idx = 0;
            const char* src = arg;
            while (*src && c_idx < 510) {
                if (*src == '\\' && *(src + 1) == 'n') {
                    vfs[slot].content[c_idx++] = '\n';
                    src += 2;
                } else {
                    vfs[slot].content[c_idx++] = *src++;
                }
            }
            vfs[slot].content[c_idx] = '\0';
            vfs[slot].size = c_idx;
            vfs[slot].is_used = 1;
            char msg[64];
            snprintf(msg, sizeof(msg), "Wrote %d bytes to '%s'.", vfs[slot].size, filename);
            shell_print(msg);
        } else {
            shell_print("VFS full (max 8 files).");
        }
    } else if (strncmp(cmd, "rm ", 3) == 0) {
        const char* filename = cmd + 3;
        while (*filename == ' ') filename++;
        int found = 0;
        for (int i = 0; i < MAX_VFILES; i++) {
            if (vfs[i].is_used && strcmp(vfs[i].name, filename) == 0) {
                vfs[i].is_used = 0;
                char msg[64];
                snprintf(msg, sizeof(msg), "Removed file '%s'.", filename);
                shell_print(msg);
                found = 1;
                break;
            }
        }
        if (!found) {
            shell_print("File not found.");
        }
    } else if (strncmp(cmd, "edit ", 5) == 0) {
        const char* filename = cmd + 5;
        while (*filename == ' ') filename++;
        int found = 0;
        for (int i = 0; i < MAX_VFILES; i++) {
            if (vfs[i].is_used && strcmp(vfs[i].name, filename) == 0) {
                strcpy(letterpad_text, vfs[i].content);
                letterpad_len = strlen(letterpad_text);
                windows[WINDOW_LETTERPAD].is_visible = 1;
                focus_window(WINDOW_LETTERPAD);
                char msg[64];
                snprintf(msg, sizeof(msg), "Loaded '%s' into LetterPad.", filename);
                shell_print(msg);
                found = 1;
                break;
            }
        }
        if (!found) {
            letterpad_text[0] = '\0';
            letterpad_len = 0;
            windows[WINDOW_LETTERPAD].is_visible = 1;
            focus_window(WINDOW_LETTERPAD);
            shell_print("Opened new file in LetterPad.");
        }
    } else if (strncmp(cmd, "run ", 4) == 0) {
        const char* filename = cmd + 4;
        while (*filename == ' ') filename++;
        int found = 0;
        for (int i = 0; i < MAX_VFILES; i++) {
            if (vfs[i].is_used && strcmp(vfs[i].name, filename) == 0) {
                found = 1;
                char line_buf[64];
                int lb_idx = 0;
                const char* ptr = vfs[i].content;
                while (*ptr) {
                    if (*ptr == '\n' || *ptr == '\r') {
                        line_buf[lb_idx] = '\0';
                        if (lb_idx > 0) {
                            run_shell_command(line_buf);
                        }
                        lb_idx = 0;
                    } else {
                        if (lb_idx < 62) {
                            line_buf[lb_idx++] = *ptr;
                        }
                    }
                    ptr++;
                }
                if (lb_idx > 0) {
                    line_buf[lb_idx] = '\0';
                    run_shell_command(line_buf);
                }
                break;
            }
        }
        if (!found) {
            shell_print("Script file not found.");
        }
    } else {
        char err_msg[128];
        snprintf(err_msg, sizeof(err_msg), "Unknown command: '%s'", cmd);
        shell_print(err_msg);
        shell_print("Type 'help' for available commands.");
    }
}

// -------------------------------------------------------------------------
// OS Actions
// -------------------------------------------------------------------------
void change_wallpaper(void) {
    current_wallpaper_style = (current_wallpaper_style + 1) % 5;
}

void reboot_system(void) {
    disable_interrupts();
    outb(0x64, 0xFE);
    struct idt_ptr null_idtp = {0, 0};
    lidt((uint32_t)&null_idtp);
    __asm__ volatile("int $3");
    while (1);
}

void shutdown_system(void) {
    disable_interrupts();
    outw(0xB004, 0x2000);
    outw(0x604, 0x2000);
    outw(0x4004, 0x3400);
    
    clear_screen_back(0x000000);
    draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 200, 270, "IT IS NOW SAFE TO TURN OFF YOUR COMPUTER", 0xFFFFFF, 1);
    draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 300, 300, "QuantumOS has shut down.", 0x888888, 1);
    swap_buffer();
    while (1) { __asm__ volatile("hlt"); }
}

// -------------------------------------------------------------------------
// Desktop Icon Drawing
// -------------------------------------------------------------------------
void draw_shell_icon(int x, int y) {
    draw_rect_back(x + 2, y + 2, 44, 44, 0x1a1a24);
    draw_rect_back(x, y, 44, 44, 0xCCCCCC);
    draw_rect_outline_back(x, y, 44, 44, 0x444444, 2);
    draw_rect_back(x + 4, y + 10, 36, 30, 0x0A0A0A);
    draw_rect_back(x, y + 8, 44, 2, 0x444444);
    draw_char_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, x + 8, y + 15, '>', 0x00FF00, 1);
    draw_char_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, x + 16, y + 15, '_', 0x00FF00, 1);
}

void draw_letterpad_icon(int x, int y) {
    draw_rect_back(x + 4, y + 2, 36, 42, 0x1a1a24);
    draw_rect_back(x + 2, y, 36, 42, 0xFFFFFF);
    draw_rect_outline_back(x + 2, y, 36, 42, 0x444444, 1);
    draw_rect_back(x + 2, y, 36, 6, 0xDD0000);
    draw_rect_back(x + 8, y + 14, 24, 1, 0x00AADD);
    draw_rect_back(x + 8, y + 20, 24, 1, 0x00AADD);
    draw_rect_back(x + 8, y + 26, 24, 1, 0x00AADD);
    draw_rect_back(x + 8, y + 32, 24, 1, 0x00AADD);
    for (int i = 0; i < 20; i++) {
        putpixel_back(x + 12 + i, y + 35 - i, 0xFFCC00);
        putpixel_back(x + 12 + i, y + 36 - i, 0x885500);
    }
}

void draw_info_icon(int x, int y) {
    draw_rect_back(x + 2, y + 2, 40, 40, 0x111111);
    draw_rect_back(x, y, 40, 40, 0x0066CC);
    draw_rect_outline_back(x, y, 40, 40, 0xFFFFFF, 1);
    draw_rect_back(x + 18, y + 8, 4, 4, 0xFFFFFF);
    draw_rect_back(x + 18, y + 16, 4, 16, 0xFFFFFF);
    draw_rect_back(x + 15, y + 16, 4, 3, 0xFFFFFF);
    draw_rect_back(x + 15, y + 30, 10, 3, 0xFFFFFF);
}

void draw_shutdown_icon(int x, int y) {
    draw_rect_back(x + 2, y + 2, 40, 40, 0x111111);
    draw_rect_back(x, y, 40, 40, 0xAA0000);
    draw_rect_outline_back(x, y, 40, 40, 0xFFFFFF, 1);
    draw_rect_back(x + 12, y + 12, 3, 16, 0xFFFFFF);
    draw_rect_back(x + 25, y + 12, 3, 16, 0xFFFFFF);
    draw_rect_back(x + 12, y + 25, 16, 3, 0xFFFFFF);
    draw_rect_back(x + 18, y + 8, 4, 12, 0xFFFFFF);
}

// -------------------------------------------------------------------------
// Window Rendering
// -------------------------------------------------------------------------
void draw_window(Window* win) {
    if (!win->is_visible) return;
    
    uint32_t title_color = win->is_focused ? 0x4A1E5C : 0x242038;
    uint32_t border_color = win->is_focused ? 0x8844AA : 0x555555;
    
    // Shadow
    draw_rect_back(win->x + 6, win->y + 6, win->w, win->h, 0x151020);
    // Body
    draw_rect_back(win->x, win->y, win->w, win->h, 0xDDDDDD);
    // Titlebar
    draw_rect_back(win->x, win->y, win->w, 24, title_color);
    draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, win->x + 8, win->y + 5, win->title, 0xFFFFFF, 1);
    // Close button
    int close_x = win->x + win->w - 20;
    int close_y = win->y + 4;
    draw_rect_back(close_x, close_y, 16, 16, 0xAA0000);
    draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, close_x + 5, close_y + 4, "x", 0xFFFFFF, 1);
    // Border
    draw_rect_outline_back(win->x, win->y, win->w, win->h, border_color, 2);
    
    // Client area
    if (win->type == WINDOW_SHELL) {
        draw_rect_back(win->x + 3, win->y + 24, win->w - 6, win->h - 27, 0x000000);
        
        if (matrix_mode) {
            for (int i = 0; i < 35; i++) {
                int cx_pos = win->x + 12 + i * 12;
                if (cx_pos + 8 >= win->x + win->w - 4) break;
                
                int cy_pos = win->y + 28 + matrix_drops[i] * 11;
                if (cy_pos + 12 >= win->y + win->h - 4) {
                    matrix_drops[i] = 0;
                    continue;
                }
                
                char rand_char = 33 + (system_ticks + i) % 94;
                draw_char_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, cx_pos, cy_pos, rand_char, 0xCCFFCC, 1);
            }
            
            if (system_ticks % 2 == 0) {
                for (int i = 0; i < 35; i++) {
                    matrix_drops[i]++;
                    if (matrix_drops[i] > 18 || (system_ticks + i) % 73 == 0)
                        matrix_drops[i] = 0;
                }
            }
            
            draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, win->x + 10, win->y + win->h - 18, "Press ANY KEY to return...", 0x00FF00, 1);
        } else {
            for (int i = 0; i < SHELL_ROWS; i++) {
                int ly = win->y + 28 + i * 11;
                if (ly > win->y + win->h - 32) break;
                draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, win->x + 8, ly, shell_lines[i], 0x00FF00, 1);
            }
            char prompt[128];
            snprintf(prompt, sizeof(prompt), "quantum_os> %s", shell_input);
            int ly = win->y + win->h - 16;
            draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, win->x + 8, ly, prompt, 0x00FF00, 1);
            
            if ((system_ticks / 9) % 2 == 0) {
                int c_x = win->x + 8 + ((int)strlen("quantum_os> ") + shell_input_idx) * 8;
                if (c_x + 8 < win->x + win->w - 4) {
                    draw_rect_back(c_x, ly, 8, 10, 0x00FF00);
                }
            }
        }
    } else if (win->type == WINDOW_LETTERPAD) {
        // Main page background
        draw_rect_back(win->x + 3, win->y + 44, win->w - 6, win->h - 47, 0xFFFFF8);
        draw_rect_back(win->x + 36, win->y + 44, 1, win->h - 47, 0xFFAA88);
        
        // Toolbar background
        draw_rect_back(win->x + 3, win->y + 24, win->w - 6, 20, 0xEEEEEE);
        draw_rect_back(win->x + 3, win->y + 44, win->w - 6, 1, 0xCCCCCC);
        
        // Save button
        draw_rect_back(win->x + 10, win->y + 27, 45, 14, 0xDDDDDD);
        draw_rect_outline_back(win->x + 10, win->y + 27, 45, 14, 0x999999, 1);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, win->x + 16, win->y + 30, "Save", 0x333333, 1);
        
        // Load button
        draw_rect_back(win->x + 65, win->y + 27, 45, 14, 0xDDDDDD);
        draw_rect_outline_back(win->x + 65, win->y + 27, 45, 14, 0x999999, 1);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, win->x + 71, win->y + 30, "Load", 0x333333, 1);
        
        int lx = win->x + 42;
        int ly = win->y + 48;
        int max_w = win->w - 50;
        int cur_x = lx;
        int cur_y = ly;
        
        for (int i = 0; i < letterpad_len; i++) {
            char c = letterpad_text[i];
            if (c == '\n') {
                cur_y += 12;
                cur_x = lx;
            } else {
                if (cur_x + 8 > lx + max_w) {
                    cur_y += 12;
                    cur_x = lx;
                }
                if (cur_y + 10 < win->y + win->h - 4) {
                    draw_char_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, cur_x, cur_y, c, 0x333333, 1);
                }
                cur_x += 8;
            }
        }
        
        if ((system_ticks / 9) % 2 == 0) {
            if (cur_y + 10 < win->y + win->h - 4) {
                draw_rect_back(cur_x, cur_y, 8, 10, 0x0000FF);
            }
        }
    } else if (win->type == WINDOW_SYSINFO) {
        draw_rect_back(win->x + 3, win->y + 24, win->w - 6, win->h - 27, 0x222233);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, win->x + 20, win->y + 38, "Quantum OS Core v1.0", 0xFFFFFF, 1);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, win->x + 20, win->y + 54, "--------------------", 0x8888AA, 1);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, win->x + 20, win->y + 70, "CPU: x86-32 Protected Mode", 0x00FF88, 1);
        char res[64];
        snprintf(res, sizeof(res), "Display: %dx%d @ 32bpp LFB", SCREEN_W, SCREEN_H);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, win->x + 20, win->y + 86, res, 0x00FF88, 1);
        char uptime[64];
        snprintf(uptime, sizeof(uptime), "Uptime: %d seconds", system_ticks / 18);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, win->x + 20, win->y + 102, uptime, 0x00FF88, 1);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, win->x + 20, win->y + 124, "For learning OS development.", 0x888888, 1);
    } else if (win->type == WINDOW_KBDVIS) {
        draw_rect_back(win->x + 3, win->y + 24, win->w - 6, win->h - 27, 0x1E1E24);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, win->x + 25, win->y + 25, "Physical Keyboard Visualizer", 0x8888AA, 1);
        
        for (int i = 0; i < 60; i++) {
            VisualKey* key = &visual_keys[i];
            int kx = win->x + key->dx;
            int ky = win->y + key->dy;
            
            int pressed = (key->scancode < 128) && key_states[key->scancode];
            
            uint32_t bg_color = pressed ? 0xFF5500 : 0x3A3A4A;
            uint32_t border_color = pressed ? 0xFFCC00 : 0x555566;
            uint32_t text_color = pressed ? 0xFFFFFF : 0xCCCCCC;
            
            draw_rect_back(kx, ky, key->dw, key->dh, bg_color);
            draw_rect_outline_back(kx, ky, key->dw, key->dh, border_color, 1);
            
            int text_x = kx + (key->dw - strlen(key->label) * 8) / 2;
            int text_y = ky + (key->dh - 8) / 2;
            draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, text_x, text_y, key->label, text_color, 1);
        }
    } else if (win->type == WINDOW_GAMESMENU) {
        draw_rect_back(win->x + 3, win->y + 24, win->w - 6, win->h - 27, 0x1A1A24);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, win->x + 20, win->y + 38, "SELECT A GAME", 0x8844AA, 1);
        
        // Chess launcher button
        int btn_x = win->x + 20;
        int btn_y = win->y + 60;
        int btn_w = 360;
        int btn_h = 70;
        int hovered = (mouse_x >= btn_x && mouse_x <= btn_x + btn_w && mouse_y >= btn_y && mouse_y <= btn_y + btn_h);
        uint32_t btn_c = hovered ? 0x2A2A3A : 0x222230;
        uint32_t outline_c = hovered ? 0xFFD700 : 0x555566;
        
        draw_rect_back(btn_x, btn_y, btn_w, btn_h, btn_c);
        draw_rect_outline_back(btn_x, btn_y, btn_w, btn_h, outline_c, 1);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, btn_x + 15, btn_y + 12, "Chess Game", 0xFFD700, 2);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, btn_x + 15, btn_y + 40, "2-Player or VS Computer (Easy/Med/Hard)", 0xCCCCCC, 1);
    } else if (win->type == WINDOW_CHESS) {
        draw_rect_back(win->x + 3, win->y + 24, win->w - 6, win->h - 27, 0x1E1E24);
        
        if (chess_mode == 0) {
            // Mode Selection
            draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, win->x + 132, win->y + 40, "CHESS CHAMPION", 0xFFD700, 2);
            
            const char* modes[4] = {
                "1. 2-Player (Pass & Play)",
                "2. VS Computer (Easy AI)",
                "3. VS Computer (Medium AI)",
                "4. VS Computer (Hard AI)"
            };
            
            for (int i = 0; i < 4; i++) {
                int btn_x = win->x + 80;
                int btn_y = win->y + 80 + i * 50;
                int btn_w = 320;
                int btn_h = 35;
                int hovered = (mouse_x >= btn_x && mouse_x <= btn_x + btn_w && mouse_y >= btn_y && mouse_y <= btn_y + btn_h);
                uint32_t btn_c = hovered ? 0x2A2A38 : 0x222230;
                uint32_t outline_c = hovered ? 0xFFD700 : 0x555566;
                uint32_t text_c = hovered ? 0xFFFFFF : 0xCCCCCC;
                
                draw_rect_back(btn_x, btn_y, btn_w, btn_h, btn_c);
                draw_rect_outline_back(btn_x, btn_y, btn_w, btn_h, outline_c, 1);
                draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, btn_x + 20, btn_y + 13, modes[i], text_c, 1);
            }
        } else {
            // Active Game
            draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, win->x + 20, win->y + 35, chess_msg, 0x00FF88, 1);
            
            // Draw board
            int bx_start = win->x + 112;
            int by_start = win->y + 64;
            for (int sy = 0; sy < 8; sy++) {
                for (int sx = 0; sx < 8; sx++) {
                    int px = bx_start + sx * 32;
                    int py = by_start + sy * 32;
                    
                    int bx, by;
                    map_chess_grid(sx, sy, &bx, &by);
                    
                    uint32_t sq_color = (sx + sy) % 2 == 0 ? 0xEEDCBE : 0xB58863;
                    if (bx == chess_sel_x && by == chess_sel_y) {
                        sq_color = 0x55AACC;
                    }
                    draw_rect_back(px, py, 32, 32, sq_color);
                    
                    uint8_t p = chess_board[by][bx];
                    if (p != 0) {
                        int side = CHESS_SIDE(p);
                        int type = CHESS_TYPE(p);
                        draw_chess_piece_icon(px + 8, py + 8, type, side);
                    }
                }
            }
            
            // Draw Reset/Menu button
            int menu_btn_x = win->x + 20;
            int menu_btn_y = win->y + 325;
            int menu_hovered = (mouse_x >= menu_btn_x && mouse_x <= menu_btn_x + 80 && mouse_y >= menu_btn_y && mouse_y <= menu_btn_y + 25);
            uint32_t menu_c = menu_hovered ? 0xAA0000 : 0x770000;
            draw_rect_back(menu_btn_x, menu_btn_y, 80, 25, menu_c);
            draw_rect_outline_back(menu_btn_x, menu_btn_y, 80, 25, 0xFFFFFF, 1);
            draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, menu_btn_x + 24, menu_btn_y + 8, "Menu", 0xFFFFFF, 1);
            
            // Mode status label
            const char* mode_lbl = "";
            if (chess_mode == 1) mode_lbl = "Mode: 2 Player";
            else if (chess_mode == 2) mode_lbl = "Mode: Easy AI";
            else if (chess_mode == 3) mode_lbl = "Mode: Medium AI";
            else if (chess_mode == 4) mode_lbl = "Mode: Hard AI";
            draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, win->x + 260, win->y + 333, mode_lbl, 0x8888AA, 1);
        }
    }
}

// -------------------------------------------------------------------------
// Boot Animation
// -------------------------------------------------------------------------
void boot_animation(void) {
    int max_frames = 100;
    
    for (int frame = 0; frame <= max_frames; frame++) {
        clear_screen_back(0x0D0D11); // Professional solid deep charcoal background
        
        // Logo text with clean fade-in
        // Fades from black (0,0,0) to soft white (224, 224, 229)
        int brightness = (frame * 224) / max_frames;
        uint32_t text_color = ((uint32_t)brightness << 16) | ((uint32_t)brightness << 8) | (uint32_t)brightness;
        
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 240, 220, "QUANTUM OS", text_color, 4);
        
        // Subtle sub-text
        int sub_brightness = (frame * 128) / max_frames;
        uint32_t sub_color = ((uint32_t)sub_brightness << 16) | ((uint32_t)sub_brightness << 8) | (uint32_t)sub_brightness;
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 310, 270, "AI Prototype Platform", sub_color, 1);
        
        // Thin elegant progress bar
        int bar_w = 200, bar_h = 3, bar_x = 300, bar_y = 340;
        draw_rect_back(bar_x, bar_y, bar_w, bar_h, 0x222228); // Outline / background
        
        int progress = (frame * bar_w) / max_frames;
        if (progress > 0) {
            draw_rect_back(bar_x, bar_y, progress, bar_h, 0xEAEAEA); // Clean silver-white progress
        }
        
        // Status logs (fade with time or display cleanly)
        uint32_t log_color = 0x8E8E93;
        if (frame < 25) {
            draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 220, 370, "[ OK ] Initializing GDT & Core Registers...", log_color, 1);
        } else if (frame < 50) {
            draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 205, 370, "[ OK ] Remapping PIC & Configuring CPU Exceptions...", log_color, 1);
        } else if (frame < 75) {
            draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 215, 370, "[ OK ] Setting up PS/2 Keyboard & Mouse Drivers...", log_color, 1);
        } else {
            draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 240, 370, "[ OK ] Launching Desktop Window Manager...", log_color, 1);
        }
        
        swap_buffer();
        // Dynamic speed delay
        for (volatile uint32_t d = 0; d < 450000; d++);
    }
}

// -------------------------------------------------------------------------
// Mouse Cursor (const data, not stack-allocated each frame)
// -------------------------------------------------------------------------
static const uint8_t cursor_bitmap[16][12] = {
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,1,1,1,1,0,0},
    {1,2,2,1,2,2,1,0,0,0,0,0},
    {1,1,1,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,1,2,2,1,0,0,0,0},
    {0,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,0,1,1,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0}
};

static void draw_cursor(int mx, int my) {
    for (int cy = 0; cy < 16; cy++) {
        for (int cx = 0; cx < 12; cx++) {
            if (cursor_bitmap[cy][cx] == 1) {
                putpixel_back(mx + cx, my + cy, 0xFFFFFF);
            } else if (cursor_bitmap[cy][cx] == 2) {
                putpixel_back(mx + cx, my + cy, 0x000000);
            }
        }
    }
}

// -------------------------------------------------------------------------
// Main Kernel Entry Point
// -------------------------------------------------------------------------
void kernel_main(void) {
    disable_interrupts();
    
    // Show boot animation
    boot_animation();
    
    // Setup IDT + PIC + all IRQ handlers
    idt_install();
    
    // Initialize PS/2 mouse
    mouse_install();
    
    // Initialize shell
    memset(shell_lines, 0, sizeof(shell_lines));
    shell_print("Welcome to QuantumOS! Type 'help'.");
    shell_print("Alt+S:Shell Alt+L:LetterPad Alt+G:Games Alt+B:Wallpaper");
    shell_print("");
    
    memset(letterpad_text, 0, sizeof(letterpad_text));
    
    // Initialize Virtual Filesystem (VFS)
    init_vfs();
    
    enable_interrupts();

    int clock_h = 0, clock_m = 0, clock_s = 0;
    
    while (1) {
        // Snapshot volatile mouse state at frame start
        int mx = mouse_x;
        int my = mouse_y;
        int m_left = mouse_left_pressed;
        int left_clicked = (m_left && !prev_mouse_left_pressed);
        
        // --- 1. Draw Wallpaper (gradient or image) ---
        if (current_wallpaper_style == 4) {
            draw_image_wallpaper_back(background1_data, BG_WIDTH, BG_HEIGHT);
        } else {
            uint32_t wall_c1 = 0, wall_c2 = 0;
            if (current_wallpaper_style == 0) {
                wall_c1 = 0x1A092A; wall_c2 = 0x4A1E5C;
            } else if (current_wallpaper_style == 1) {
                wall_c1 = 0x05131C; wall_c2 = 0x1A354C;
            } else if (current_wallpaper_style == 2) {
                wall_c1 = 0x260710; wall_c2 = 0x5C0D24;
            } else {
                wall_c1 = 0x101518; wall_c2 = 0x2A3238;
            }
            draw_gradient_vertical_back(0, 0, SCREEN_W, 560, wall_c1, wall_c2);
        }
        
        // --- 2. Desktop Icons ---
        int icon_hover = -1;
        if (mx >= 15 && mx <= 75 && my >= 25 && my <= 95) icon_hover = 0;
        else if (mx >= 15 && mx <= 75 && my >= 105 && my <= 175) icon_hover = 1;
        else if (mx >= 15 && mx <= 75 && my >= 185 && my <= 255) icon_hover = 2;
        else if (mx >= 15 && mx <= 75 && my >= 265 && my <= 335) icon_hover = 3;
        
        if (icon_hover == 0) { draw_rect_back(10, 20, 60, 70, 0x33225577); draw_rect_outline_back(10, 20, 60, 70, 0x8844AA, 1); }
        draw_shell_icon(18, 25);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 16, 75, "Terminal", 0xFFFFFF, 1);
        
        if (icon_hover == 1) { draw_rect_back(10, 100, 60, 70, 0x33225577); draw_rect_outline_back(10, 100, 60, 70, 0x8844AA, 1); }
        draw_letterpad_icon(18, 105);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 13, 155, "LetterPad", 0xFFFFFF, 1);

        if (icon_hover == 2) { draw_rect_back(10, 180, 60, 70, 0x33225577); draw_rect_outline_back(10, 180, 60, 70, 0x8844AA, 1); }
        draw_info_icon(20, 185);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 12, 235, "Sys Info", 0xFFFFFF, 1);

        if (icon_hover == 3) { draw_rect_back(10, 260, 60, 70, 0x33225577); draw_rect_outline_back(10, 260, 60, 70, 0x8844AA, 1); }
        draw_shutdown_icon(20, 265);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 15, 315, "PowerOff", 0xFFFFFF, 1);

        // --- 3. Window Management ---
        int win_draw_order[NUM_WINDOWS];
        int draw_idx = 0;
        for (int i = 0; i < NUM_WINDOWS; i++) {
            if (i != active_win_idx) {
                win_draw_order[draw_idx++] = i;
            }
        }
        if (active_win_idx >= 0 && active_win_idx < NUM_WINDOWS) {
            win_draw_order[draw_idx++] = active_win_idx;
        }

        if (left_clicked) {
            int handled = 0;
            for (int i = NUM_WINDOWS - 1; i >= 0; i--) {
                int w_idx = win_draw_order[i];
                Window* win = &windows[w_idx];
                if (!win->is_visible) continue;
                
                int cx = win->x + win->w - 20;
                int cy = win->y + 4;
                if (mx >= cx && mx <= cx + 16 && my >= cy && my <= cy + 16) {
                    win->is_visible = 0; handled = 1; break;
                }
                
                // Games Menu click checks
                if (win->type == WINDOW_GAMESMENU) {
                    if (mx >= win->x + 20 && mx <= win->x + 380 && my >= win->y + 60 && my <= win->y + 130) {
                        windows[WINDOW_CHESS].is_visible = 1;
                        focus_window(WINDOW_CHESS);
                        handled = 1;
                        break;
                    }
                }
                
                // Chess click checks
                if (win->type == WINDOW_CHESS) {
                    if (chess_mode == 0) {
                        if (mx >= win->x + 80 && mx <= win->x + 400) {
                            if (my >= win->y + 80 && my <= win->y + 115) {
                                init_chess_game(1);
                                handled = 1; focus_window(w_idx); break;
                            }
                            if (my >= win->y + 130 && my <= win->y + 165) {
                                init_chess_game(2);
                                handled = 1; focus_window(w_idx); break;
                            }
                            if (my >= win->y + 180 && my <= win->y + 215) {
                                init_chess_game(3);
                                handled = 1; focus_window(w_idx); break;
                            }
                            if (my >= win->y + 230 && my <= win->y + 265) {
                                init_chess_game(4);
                                handled = 1; focus_window(w_idx); break;
                            }
                        }
                    } else {
                        // Reset/Menu button
                        if (mx >= win->x + 20 && mx <= win->x + 100 && my >= win->y + 325 && my <= win->y + 350) {
                            chess_mode = 0;
                            handled = 1; focus_window(w_idx); break;
                        }
                        
                        // Board click
                        int bx_start = win->x + 112;
                        int by_start = win->y + 64;
                        if (mx >= bx_start && mx < bx_start + 256 && my >= by_start && my < by_start + 256) {
                            int click_sx = (mx - bx_start) / 32;
                            int click_sy = (my - by_start) / 32;
                            int click_bx, click_by;
                            map_chess_grid(click_sx, click_sy, &click_bx, &click_by);
                            handle_chess_click(click_bx, click_by);
                            handled = 1; focus_window(w_idx); break;
                        }
                    }
                }
                
                // Toolbar buttons check for LetterPad
                if (win->type == WINDOW_LETTERPAD) {
                    // Save button: x: [win->x + 10, win->x + 55], y: [win->y + 27, win->y + 41]
                    if (mx >= win->x + 10 && mx <= win->x + 55 && my >= win->y + 27 && my <= win->y + 41) {
                        int slot = -1;
                        for (int j = 0; j < MAX_VFILES; j++) {
                            if (vfs[j].is_used && strcmp(vfs[j].name, "note.txt") == 0) {
                                slot = j;
                                break;
                            }
                        }
                        if (slot == -1) {
                            for (int j = 0; j < MAX_VFILES; j++) {
                                if (!vfs[j].is_used) {
                                    slot = j;
                                    break;
                                }
                            }
                        }
                        if (slot != -1) {
                            strcpy(vfs[slot].name, "note.txt");
                            strcpy(vfs[slot].content, letterpad_text);
                            vfs[slot].size = letterpad_len;
                            vfs[slot].is_used = 1;
                            shell_print("Saved LetterPad text to 'note.txt'.");
                        } else {
                            shell_print("Failed to save: VFS is full!");
                        }
                        handled = 1;
                        focus_window(w_idx);
                        break;
                    }
                    
                    // Load button: x: [win->x + 65, win->x + 110], y: [win->y + 27, win->y + 41]
                    if (mx >= win->x + 65 && mx <= win->x + 110 && my >= win->y + 27 && my <= win->y + 41) {
                        int slot = -1;
                        for (int j = 0; j < MAX_VFILES; j++) {
                            if (vfs[j].is_used && strcmp(vfs[j].name, "note.txt") == 0) {
                                slot = j;
                                break;
                            }
                        }
                        if (slot != -1) {
                            strcpy(letterpad_text, vfs[slot].content);
                            letterpad_len = vfs[slot].size;
                            shell_print("Loaded 'note.txt' into LetterPad.");
                        } else {
                            shell_print("No 'note.txt' found in VFS!");
                        }
                        handled = 1;
                        focus_window(w_idx);
                        break;
                    }
                }
                if (mx >= win->x && mx <= win->x + win->w && my >= win->y && my <= win->y + 24) {
                    win->is_dragged = 1;
                    drag_offset_x = mx - win->x;
                    drag_offset_y = my - win->y;
                    focus_window(w_idx); handled = 1; break;
                }
                if (mx >= win->x && mx <= win->x + win->w && my >= win->y + 24 && my <= win->y + win->h) {
                    focus_window(w_idx); handled = 1; break;
                }
            }
            
            if (!handled) {
                if (icon_hover == 0) { windows[WINDOW_SHELL].is_visible = 1; focus_window(WINDOW_SHELL); }
                else if (icon_hover == 1) { windows[WINDOW_LETTERPAD].is_visible = 1; focus_window(WINDOW_LETTERPAD); }
                else if (icon_hover == 2) { windows[WINDOW_SYSINFO].is_visible = 1; focus_window(WINDOW_SYSINFO); }
                else if (icon_hover == 3) shutdown_system();
            }
        }
        
        // Window drag
        for (int i = 0; i < NUM_WINDOWS; i++) {
            if (windows[i].is_dragged) {
                if (m_left) {
                    windows[i].x = mx - drag_offset_x;
                    windows[i].y = my - drag_offset_y;
                    if (windows[i].x < -50) windows[i].x = -50;
                    if (windows[i].x > 750) windows[i].x = 750;
                    if (windows[i].y < 0) windows[i].y = 0;
                    if (windows[i].y > 520) windows[i].y = 520;
                } else {
                    windows[i].is_dragged = 0;
                }
            }
        }

        // Draw windows
        for (int i = 0; i < NUM_WINDOWS; i++) {
            draw_window(&windows[win_draw_order[i]]);
        }
        
        // --- 4. Start Menu ---
        if (left_clicked && start_menu_open) {
            if (!(mx >= 10 && mx <= 170 && my >= 378 && my <= 556)) {
                start_menu_open = 0;
            }
        }
        
        if (start_menu_open) {
            draw_rect_back(14, 368, 160, 192, 0x0C0B18);
            draw_rect_back(10, 364, 160, 192, 0x1A1A24);
            draw_rect_outline_back(10, 364, 160, 192, 0x8844AA, 1);
            
            const char* options[6] = { " Terminal", " LetterPad", " Games Menu", " Visualizer", " System Info", " Power Off" };
            
            for (int i = 0; i < 6; i++) {
                int item_y = 369 + i * 32;
                int hovered = (mx >= 10 && mx <= 170 && my >= item_y && my < item_y + 32);
                
                if (hovered) {
                    draw_rect_back(12, item_y, 156, 30, 0x8844AA);
                    draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 20, item_y + 10, options[i], 0xFFFFFF, 1);
                    if (left_clicked) {
                        start_menu_open = 0;
                        if (i == 0) { windows[WINDOW_SHELL].is_visible = 1; focus_window(WINDOW_SHELL); }
                        else if (i == 1) { windows[WINDOW_LETTERPAD].is_visible = 1; focus_window(WINDOW_LETTERPAD); }
                        else if (i == 2) { windows[WINDOW_GAMESMENU].is_visible = 1; focus_window(WINDOW_GAMESMENU); }
                        else if (i == 3) { windows[WINDOW_KBDVIS].is_visible = 1; focus_window(WINDOW_KBDVIS); }
                        else if (i == 4) { windows[WINDOW_SYSINFO].is_visible = 1; focus_window(WINDOW_SYSINFO); }
                        else if (i == 5) shutdown_system();
                    }
                } else {
                    draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 20, item_y + 10, options[i], 0xCCCCCC, 1);
                }
            }
        }

        // --- 5. Taskbar ---
        draw_rect_back(0, 560, SCREEN_W, 40, 0x16161D);
        draw_rect_back(0, 560, SCREEN_W, 1, 0x555566);
        
        int start_hover = (mx >= 10 && mx <= 90 && my >= 566 && my <= 594);
        uint32_t start_c = start_menu_open ? 0x8844AA : (start_hover ? 0x444455 : 0x2E2E3A);
        draw_rect_back(10, 566, 80, 28, start_c);
        draw_rect_outline_back(10, 566, 80, 28, 0x8844AA, 1);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 30, 576, "Start", 0xFFFFFF, 1);
        
        if (left_clicked && start_hover) {
            start_menu_open = !start_menu_open;
        }

        int task_sh_hover = (mx >= 100 && mx <= 190 && my >= 566 && my <= 594);
        int task_lp_hover = (mx >= 200 && mx <= 290 && my >= 566 && my <= 594);
        int task_gm_hover = (mx >= 300 && mx <= 390 && my >= 566 && my <= 594);
        int task_kv_hover = (mx >= 400 && mx <= 490 && my >= 566 && my <= 594);
        int task_ch_hover = (mx >= 500 && mx <= 590 && my >= 566 && my <= 594);
        
        uint32_t sh_btn_c = windows[WINDOW_SHELL].is_visible ? 0x8844AA : (task_sh_hover ? 0x444455 : 0x222233);
        draw_rect_back(100, 566, 90, 28, sh_btn_c);
        draw_rect_outline_back(100, 566, 90, 28, 0x555566, 1);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 112, 576, "Terminal", 0xFFFFFF, 1);
        if (left_clicked && task_sh_hover) toggle_window(WINDOW_SHELL);
 
        uint32_t lp_btn_c = windows[WINDOW_LETTERPAD].is_visible ? 0x8844AA : (task_lp_hover ? 0x444455 : 0x222233);
        draw_rect_back(200, 566, 90, 28, lp_btn_c);
        draw_rect_outline_back(200, 566, 90, 28, 0x555566, 1);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 206, 576, "LetterPad", 0xFFFFFF, 1);
        if (left_clicked && task_lp_hover) toggle_window(WINDOW_LETTERPAD);
 
        uint32_t gm_btn_c = windows[WINDOW_GAMESMENU].is_visible ? 0x8844AA : (task_gm_hover ? 0x444455 : 0x222233);
        draw_rect_back(300, 566, 90, 28, gm_btn_c);
        draw_rect_outline_back(300, 566, 90, 28, 0x555566, 1);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 322, 576, "Games", 0xFFFFFF, 1);
        if (left_clicked && task_gm_hover) toggle_window(WINDOW_GAMESMENU);
 
        uint32_t kv_btn_c = windows[WINDOW_KBDVIS].is_visible ? 0x8844AA : (task_kv_hover ? 0x444455 : 0x222233);
        draw_rect_back(400, 566, 90, 28, kv_btn_c);
        draw_rect_outline_back(400, 566, 90, 28, 0x555566, 1);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 405, 576, "Visualizer", 0xFFFFFF, 1);
        if (left_clicked && task_kv_hover) toggle_window(WINDOW_KBDVIS);

        uint32_t ch_btn_c = windows[WINDOW_CHESS].is_visible ? 0x8844AA : (task_ch_hover ? 0x444455 : 0x222233);
        draw_rect_back(500, 566, 90, 28, ch_btn_c);
        draw_rect_outline_back(500, 566, 90, 28, 0x555566, 1);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 526, 576, "Chess", 0xFFFFFF, 1);
        if (left_clicked && task_ch_hover) toggle_window(WINDOW_CHESS);

        // Clock
        if (system_ticks % 18 == 0) {
            read_rtc(&clock_h, &clock_m, &clock_s);
        }
        char clock_str[16];
        snprintf(clock_str, sizeof(clock_str), "%d:%d:%d", clock_h, clock_m, clock_s);
        draw_rect_back(700, 566, 90, 28, 0x222233);
        draw_rect_outline_back(700, 566, 90, 28, 0x555566, 1);
        draw_string_in_buffer((uint32_t*)BACKBUFFER_ADDR, SCREEN_W, SCREEN_H, 715, 576, clock_str, 0xFFFFFF, 1);

        // --- 6. Mouse Cursor ---
        draw_cursor(mx, my);

        prev_mouse_left_pressed = m_left;

        // --- 7. Flush to screen ---
        swap_buffer();
        
        // Halt until next interrupt (saves CPU, allows IRQs to fire)
        __asm__ volatile("hlt");
    }
}
