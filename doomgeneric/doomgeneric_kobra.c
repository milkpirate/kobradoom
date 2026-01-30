// doomgeneric_kobra.c – Fullscreen Framebuffer + USB HID keyboard for doomgeneric

#include "doomgeneric.h"
#include "doomkeys.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <linux/fb.h>
#include <errno.h>
#include <arm_neon.h>

#include "usb_hid_keys.h"

/* ─────────────────────────────────────────────────────────────────────────
   Framebuffer
   ───────────────────────────────────────────────────────────────────────── */
static const char *framebuffer_dev_path = "/dev/fb0";

static uint32_t *fb_mem = NULL;

static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;

static uint32_t x_lut[1024]; // bug enough for target resolution (e.g. 840)

static void init_x_lut(int dst_w) {
    uint32_t x_step = (DOOMGENERIC_RESX << 16) / dst_w;
    uint32_t cur_x = 0;
    for (int x = 0; x < dst_w; x++) {
        x_lut[x] = cur_x >> 16;
        cur_x += x_step;
    }
}


static uint32_t rotated_buffer[DOOMGENERIC_RESX * DOOMGENERIC_RESY];


/* ─────────────────────────────────────────────────────────────────────────
   Timing
   ───────────────────────────────────────────────────────────────────────── */
static struct timespec start_time;

/* ─────────────────────────────────────────────────────────────────────────
   HID
   ───────────────────────────────────────────────────────────────────────── */
static const char *default_hid_dev_path = "/dev/hidraw0";
static int hid_fd = -1;

#define KEYQUEUE_SIZE 16
static unsigned short g_key_queue[KEYQUEUE_SIZE];
static unsigned int g_key_queue_write_index = 0;
static unsigned int g_key_queue_read_index = 0;

static unsigned char hid_to_doom(const unsigned char hid_code) {
    // Map USB HID Usage ID to a Doom Key.
    // This mapping is based on the HID Usage Tables for keyboards.
    // See: https://www.usb.org/sites/default/files/documents/hut1_12v2.pdf (Chapter 10)

    // Letters a-z (HID 0x04-0x1D)
    if (hid_code >= KEY_HID_A && hid_code <= KEY_HID_Z) {
        return 'a' + (hid_code - KEY_HID_A);
    }

    // Numbers 1-9 (HID 0x1E-0x26)...
    if (hid_code >= KEY_HID_1 && hid_code <= KEY_HID_9) {
        return '1' + (hid_code - KEY_HID_1);
    }

    // ...and 0 (HID 0x27) are standard in HID tables.
    if (hid_code == KEY_HID_0) {
        return '0';
    }

    // Special Keys
    switch (hid_code) {
        // Navigation Keys
        case KEY_HID_RIGHT: return KEY_RIGHTARROW;
        case KEY_HID_LEFT: return KEY_LEFTARROW;
        case KEY_HID_DOWN: return KEY_DOWNARROW;
        case KEY_HID_UP: return KEY_UPARROW;

            // Function Keys
        case KEY_HID_F1: return KEY_F1;
        case KEY_HID_F2: return KEY_F2;
        case KEY_HID_F3: return KEY_F3;
        case KEY_HID_F4: return KEY_F4;
        case KEY_HID_F5: return KEY_F5;
        case KEY_HID_F6: return KEY_F6;
        case KEY_HID_F7: return KEY_F7;
        case KEY_HID_F8: return KEY_F8;
        case KEY_HID_F9: return KEY_F9;
        case KEY_HID_F10: return KEY_F10;
        case KEY_HID_F11: return KEY_F11;
        case KEY_HID_F12: return KEY_F12;

            // System Keys
        case KEY_HID_ENTER: return KEY_ENTER;
        case KEY_HID_ESC: return KEY_ESCAPE;
        case KEY_HID_BACKSPACE: return KEY_BACKSPACE;
        case KEY_HID_TAB: return KEY_TAB;
        case KEY_HID_SPACE: return KEY_USE;
        case KEY_HID_MINUS: return KEY_MINUS;
        case KEY_HID_SYSRQ: return KEY_PRTSCR;
        case KEY_HID_SCROLLLOCK: return KEY_SCRLCK;
        case KEY_HID_PAUSE: return KEY_PAUSE;
        case KEY_HID_CAPSLOCK: return KEY_CAPSLOCK;

            // Modifiers
        case KEY_HID_LEFTCTRL: return KEY_FIRE;// Left Ctrl
        case KEY_HID_RIGHTCTRL: return KEY_RCTRL;
        case KEY_HID_RIGHTSHIFT: // Left Shift
        case KEY_HID_LEFTSHIFT: return KEY_RSHIFT;

        case KEY_HID_LEFTALT: return KEY_LALT;
        case KEY_HID_RIGHTALT: return KEY_RALT;

            // Editing Keys
        case KEY_HID_INSERT: return KEY_INS;
        case KEY_HID_HOME: return KEY_HOME;
        case KEY_HID_PAGEUP: return KEY_PGUP;
        case KEY_HID_DELETE: return KEY_DEL;
        case KEY_HID_END: return KEY_END;
        case KEY_HID_PAGEDOWN: return KEY_PGDN;

            // Keypad
        case KEY_HID_NUMLOCK: return KEY_NUMLOCK;
        case KEY_HID_KPSLASH: return KEYP_DIVIDE;
        case KEY_HID_KPASTERISK: return KEYP_MULTIPLY;
        case KEY_HID_KPMINUS: return KEYP_MINUS;
        case KEY_HID_KPPLUS: return KEYP_PLUS;
        case KEY_HID_EQUAL: return KEY_EQUALS;
        case KEY_HID_KPENTER: return KEYP_ENTER;

        case KEY_HID_KP1: return KEYP_1;
        case KEY_HID_KP2: return KEYP_2;
        case KEY_HID_KP3: return KEYP_3;
        case KEY_HID_KP4: return KEYP_4;
        case KEY_HID_KP5: return KEYP_5;
        case KEY_HID_KP6: return KEYP_6;
        case KEY_HID_KP7: return KEYP_7;
        case KEY_HID_KP8: return KEYP_8;
        case KEY_HID_KP9: return KEYP_9;
        case KEY_HID_KP0: return KEYP_0;

        case KEY_HID_KPDOT: return KEYP_PERIOD;

        default:   return 0;
    }
}

static void add_key_to_queue(const int pressed, const unsigned char key) {
    const auto key_data = pressed << 8 | key;

    g_key_queue[g_key_queue_write_index] = key_data;
    g_key_queue_write_index++;
    g_key_queue_write_index %= KEYQUEUE_SIZE;
}

static void handle_modifier_changes(const uint8_t prev_mod, const uint8_t cur_mod) {
    const auto diff = prev_mod ^ cur_mod;
    if (!diff) return;

    for (int bit = 0; bit < 8; ++bit) {
        const auto mask = 1 << bit;
        if (!(diff & mask)) continue;

        auto pressed = (cur_mod & mask) != 0;
        auto hid_mod_code = KEY_HID_LEFTCTRL + bit; // KEY_HID_LEFTCTRL (0xE0).. KEY_HID_RIGHTMETA (0xE7) are modifiers
        add_key_to_queue(pressed, hid_to_doom(hid_mod_code));
    }
}

static bool contains_keycode(const uint8_t keys[6], const uint8_t code) {
    for (int i = 0; i < 6; ++i) {
        if (keys[i] == code) return true;
    }
    return false;
}

static void handle_key_changes(const uint8_t prev_keys[6], const uint8_t cur_keys[6]) {
    // Pressed: present now, not present before
    for (int i = 0; i < 6; ++i) {
        const auto code = cur_keys[i];
        if (code <= 3) continue; // 0=no key; 1..3=error/rollover in boot protocol practice
        if (!contains_keycode(prev_keys, code)) {
            add_key_to_queue(1, hid_to_doom(code));
        }
    }

    // Released: present before, not present now
    for (int i = 0; i < 6; ++i) {
        const auto code = prev_keys[i];
        if (code <= 3) continue;
        if (!contains_keycode(cur_keys, code)) {
            add_key_to_queue(0, hid_to_doom(code));
        }
    }
}

static void read_hid_report_queue(void) {
    static uint8_t latest_report[8] = {0};

    while(true) {
        uint8_t raw_buf[64];

        auto n = read(hid_fd, raw_buf, sizeof(raw_buf));
        if (n < 0) {
            if (errno == EAGAIN) break;
            return; // optionally log errno here
        }
        if (n == 0) break;

        // Some devices prepend a Report ID byte. If present, skip it.
        const uint8_t *report = NULL;

        if (n == 8) {
            report = raw_buf;
        } else if (n >= 9) {
            report = raw_buf + 1;
        } else {
            continue; // too short to be a keyboard report
        }

        // Boot keyboard report format (8 bytes):
        // [0]=modifier bits, [1]=reserved, [2..7]=6 simultaneous keycodes. [web:156]
        const auto cur_mod = report[0];
        const auto *cur_keys = &report[2];

        const auto prev_mod = latest_report[0];
        const auto *prev_keys = &latest_report[2];

        handle_modifier_changes(prev_mod, cur_mod);
        handle_key_changes(prev_keys, cur_keys);

        memcpy(latest_report, report, sizeof latest_report);
    }
}

/* ─────────────────────────────────────────────────────────────────────────
   doomgeneric interface
   ───────────────────────────────────────────────────────────────────────── */

void DG_DrawFrame(void) {
    const int src_w = DOOMGENERIC_RESX;
    const int src_h = DOOMGENERIC_RESY;
    const int dst_w = vinfo.xres;
    const int dst_h = vinfo.yres;
    const uint32_t fb_stride = finfo.line_length / sizeof(uint32_t);

    // --- STEP 1: 180° Rotation (2-Way Interleaved NEON) ---
    // We process 8 pixels per iteration to hide memory latency.
    // Reading forward from s_ptr, writing backward from d_ptr.
    uint32_t* s_ptr = (uint32_t*)DG_ScreenBuffer;
    uint32_t* d_ptr = rotated_buffer + (src_w * src_h) - 8;

    for (int i = 0; i < (src_w * src_h) / 8; i++) {
        // Load two 128-bit vectors (8 pixels total)
        uint32x4_t v1 = vld1q_u32(s_ptr);
        uint32x4_t v2 = vld1q_u32(s_ptr + 4);

        // Flip vectors: [A,B,C,D] -> [D,C,B,A]
        // vrev64 swaps lanes within 64-bit halves, vcombine reorders the halves
        uint32x4_t f1 = vcombine_u32(vrev64_u32(vget_high_u32(v1)), vrev64_u32(vget_low_u32(v1)));
        uint32x4_t f2 = vcombine_u32(vrev64_u32(vget_high_u32(v2)), vrev64_u32(vget_low_u32(v2)));

        // Store flipped vectors in reverse order
        vst1q_u32(d_ptr + 4, f1);
        vst1q_u32(d_ptr, f2);

        s_ptr += 8;
        d_ptr -= 8;
    }

    // --- STEP 2: Scaling with Precomputed LUT & NEON Store ---
    const uint32_t y_step = (src_h << 16) / dst_h;
    uint32_t cur_y_fixed = 0;

    for (int y = 0; y < dst_h; y++) {
        uint32_t* src_line = rotated_buffer + (cur_y_fixed >> 16) * src_w;
        uint32_t* dst_line = fb_mem + y * fb_stride;

        int x = 0;
        // Unroll by 4: Use LUT for gathering, NEON for wide 128-bit stores
        for (; x <= dst_w - 4; x += 4) {
            // Initialize vector with the first pixel to avoid uninitialized stalls
            uint32x4_t v_pix = vdupq_n_u32(src_line[x_lut[x]]);
            v_pix = vsetq_lane_u32(src_line[x_lut[x+1]], v_pix, 1);
            v_pix = vsetq_lane_u32(src_line[x_lut[x+2]], v_pix, 2);
            v_pix = vsetq_lane_u32(src_line[x_lut[x+3]], v_pix, 3);

            // Optimized store directly to the framebuffer
            vst1q_u32(&dst_line[x], v_pix);
        }

        // Handle remaining pixels if width is not a multiple of 4
        for (; x < dst_w; x++) {
            dst_line[x] = src_line[x_lut[x]];
        }
        cur_y_fixed += y_step;
    }
}

int DG_GetKey(int* pressed, unsigned char* doomKey) {
    read_hid_report_queue();

    //key queue is empty
    if (g_key_queue_read_index == g_key_queue_write_index) {
        return 0;
    }

    const auto keyData = g_key_queue[g_key_queue_read_index];
    g_key_queue_read_index++;
    g_key_queue_read_index %= KEYQUEUE_SIZE;

    *pressed = keyData >> 8;
    *doomKey = keyData & 0xFF;

    return 1;
}

void DG_Init(void) {
    /* ── Force unbuffered output so printf works instantly ── */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* ── Open framebuffer ── */
    const auto fb_fd = open(framebuffer_dev_path, O_RDWR);
    if (fb_fd < 0) {
        fprintf(stderr, "cannot open framebuffer device file: %s - %s\n", framebuffer_dev_path, strerror(errno));
    }

    // Get screen info
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0)
    {
        fprintf(stderr, "cannot ioctl framebuffer: %s\n", strerror(errno));
    }

    fb_mem = mmap(NULL, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        fprintf(stderr, "cannot memory map framebuffer: %s\n", strerror(errno));
    }

    init_x_lut(vinfo.xres);

    /* ── Open keyboard ── */
    const auto *hid_dev = getenv("DOOM_KBDEV");
    if (!hid_dev) {
        hid_dev = default_hid_dev_path;
    }

    hid_fd = open(hid_dev, O_RDONLY | O_NONBLOCK);
    if (hid_fd < 0) {
        fprintf(stderr, "cannot open HID device %s: %s\n", hid_dev, strerror(errno));
    }

    clock_gettime(CLOCK_MONOTONIC, &start_time);
}

void DG_SleepMs(const uint32_t ms) {
    usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec - start_time.tv_sec) * 1000 + (ts.tv_nsec - start_time.tv_nsec) / 1000000;
}

void DG_SetWindowTitle(const char *title) {
    (void)title;
}

/* ─────────────────────────────────────────────────────────────────────────
   Entrypoint
   ───────────────────────────────────────────────────────────────────────── */

int main(const int argc, const char **argv) {
    doomgeneric_Create(argc, argv);
    while(true) {
        doomgeneric_Tick();
    }
}
