// doomgeneric_kobra.c – Fullscreen Framebuffer + USB HID keyboard for doomgeneric

#include "doomgeneric.h"
#include "doomkeys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <errno.h>

#include "doomtype.h"

/* ─────────────────────────────────────────────────────────────────────────
   Framebuffer
   ───────────────────────────────────────────────────────────────────────── */
static uint32_t *fb_mem = NULL;
static struct fb_var_screeninfo vinfo;
static struct fb_fix_screeninfo finfo;

/* ─────────────────────────────────────────────────────────────────────────
   Timing
   ───────────────────────────────────────────────────────────────────────── */
static struct timeval start_time;

/* ─────────────────────────────────────────────────────────────────────────
   HID
   ───────────────────────────────────────────────────────────────────────── */
static const char *default_hid_dev_path = "/dev/hidraw0";
static const char *framebuffer_dev_path = "/dev/fb0";
#define HID_DEBUG 1

typedef struct {
    int pressed;
    unsigned char key;
} keyEvent;

#define KEYQUEUE_SIZE 16
static keyEvent keyQueue[KEYQUEUE_SIZE];
static unsigned int keyQueueWriteIndex = 0;
static unsigned int keyQueueReadIndex = 0;

static int hid_fd = -1;
static unsigned char latest_report[8] = {0};

// Map USB HID Usage ID to a Doom Key.
// This mapping is based on the HID Usage Tables for keyboards.
// See: https://www.usb.org/sites/default/files/documents/hut1_12v2.pdf (Chapter 10)
static unsigned char hid_to_doom(unsigned char hid_code) {
    unsigned char mapped = 0;
    char special[64] = {0};

    // Letters a-z (HID 0x04-0x1D)
    if (hid_code >= 0x04 && hid_code <= 0x1D) {
        mapped = 'a' + (hid_code - 0x04);

        switch (mapped) {
            // Alternative navigation (strafe)
            case 'w': mapped = KEY_UPARROW;    sprintf(special, "KEY_UPARROW");    break;
            case 'a': mapped = KEY_STRAFE_L;   sprintf(special, "KEY_STRAFE_L");   break;
            case 's': mapped = KEY_DOWNARROW; sprintf(special, "KEY_DOWNARROW"); break;
            case 'd': mapped = KEY_STRAFE_R;   sprintf(special, "KEY_STRAFE_R");   break;

            case 'e': mapped = KEY_USE; sprintf(special, "KEY_USE"); break;
        }
    }
    // Numbers 1-9, 0 (HID 0x1E-0x27)
    else if (hid_code >= 0x1E && hid_code <= 0x26) { // 1-9
        mapped = '1' + (hid_code - 0x1E);
    }
    else if (hid_code == 0x27) { // 0
        mapped = '0';
    }
    // Special Keys
    else {
        switch (hid_code) {
            // Navigation Keys
            case 0x4F: mapped = KEY_RIGHTARROW; sprintf(special, "KEY_RIGHTARROW"); break;
            case 0x50: mapped = KEY_LEFTARROW;  sprintf(special, "KEY_LEFTARROW");  break;
            case 0x51: mapped = KEY_DOWNARROW;  sprintf(special, "KEY_DOWNARROW");  break;
            case 0x52: mapped = KEY_UPARROW;    sprintf(special, "KEY_UPARROW");    break;

                // Function Keys
            case 0x3A: mapped = KEY_F1;         sprintf(special, "KEY_F1");         break;
            case 0x3B: mapped = KEY_F2;         sprintf(special, "KEY_F2");         break;
            case 0x3C: mapped = KEY_F3;         sprintf(special, "KEY_F3");         break;
            case 0x3D: mapped = KEY_F4;         sprintf(special, "KEY_F4");         break;
            case 0x3E: mapped = KEY_F5;         sprintf(special, "KEY_F5");         break;
            case 0x3F: mapped = KEY_F6;         sprintf(special, "KEY_F6");         break;
            case 0x40: mapped = KEY_F7;         sprintf(special, "KEY_F7");         break;
            case 0x41: mapped = KEY_F8;         sprintf(special, "KEY_F8");         break;
            case 0x42: mapped = KEY_F9;         sprintf(special, "KEY_F9");         break;
            case 0x43: mapped = KEY_F10;        sprintf(special, "KEY_F10");        break;
            case 0x44: mapped = KEY_F11;        sprintf(special, "KEY_F11");        break;
            case 0x45: mapped = KEY_F12;        sprintf(special, "KEY_F12");        break;

                // System Keys
            case 0x28: mapped = KEY_ENTER;      sprintf(special, "KEY_ENTER");      break;
            case 0x29: mapped = KEY_ESCAPE;     sprintf(special, "KEY_ESCAPE");     break;
            case 0x2A: mapped = KEY_BACKSPACE;  sprintf(special, "KEY_BACKSPACE");  break;
            case 0x2B: mapped = KEY_TAB;        sprintf(special, "KEY_TAB");        break;
            case 0x2C: mapped = KEY_USE;        sprintf(special, "KEY_USE");        break;
            case 0x38: mapped = KEY_MINUS;      sprintf(special, "KEY_MINUS");      break;
            case 0x46: mapped = KEY_PRTSCR;     sprintf(special, "KEY_PRTSCR");     break;
            case 0x47: mapped = KEY_SCRLCK;     sprintf(special, "KEY_SCRLCK");     break;
            case 0x48: mapped = KEY_PAUSE;      sprintf(special, "KEY_PAUSE");      break;
            case 0x39: mapped = KEY_CAPSLOCK;   sprintf(special, "KEY_CAPSLOCK");   break;

                // Modifiers
            case 0xE0: // Left Ctrl
            case 0xE4: mapped = KEY_RCTRL;      sprintf(special, "KEY_RCTRL");      break;
            case 0xE1: // Left Shift
            case 0xE5: mapped = KEY_RSHIFT;     sprintf(special, "KEY_RSHIFT");     break;
            case 0xE2: mapped = KEY_LALT;       sprintf(special, "KEY_LALT");       break;
            case 0xE6: mapped = KEY_RALT;       sprintf(special, "KEY_RALT");       break;

                // Editing Keys
            case 0x49: mapped = KEY_INS;        sprintf(special, "KEY_INS");        break;
            case 0x4A: mapped = KEY_HOME;       sprintf(special, "KEY_HOME");       break;
            case 0x4B: mapped = KEY_PGUP;       sprintf(special, "KEY_PGUP");       break;
            case 0x4C: mapped = KEY_DEL;        sprintf(special, "KEY_DEL");        break;
            case 0x4D: mapped = KEY_END;        sprintf(special, "KEY_END");        break;
            case 0x4E: mapped = KEY_PGDN;       sprintf(special, "KEY_PGDN");       break;

                // Keypad
            case 0x53: mapped = KEY_NUMLOCK;    sprintf(special, "KEY_NUMLOCK");    break;
            case 0x54: mapped = KEYP_DIVIDE;    sprintf(special, "KEYP_DIVIDE");    break;
            case 0x55: mapped = KEYP_MULTIPLY;  sprintf(special, "KEYP_MULTIPLY");  break;
            case 0x56: mapped = KEYP_MINUS;     sprintf(special, "KEYP_MINUS");     break;
            case 0x57: mapped = KEYP_PLUS;      sprintf(special, "KEYP_PLUS");      break;
            case 0x58: mapped = KEYP_ENTER;     sprintf(special, "KEYP_ENTER");     break;

            case 0x59: mapped = KEYP_1;         sprintf(special, "KEYP_1");         break;
            case 0x5A: mapped = KEYP_2;         sprintf(special, "KEYP_2");         break;
            case 0x5B: mapped = KEYP_3;         sprintf(special, "KEYP_3");         break;
            case 0x5C: mapped = KEYP_4;         sprintf(special, "KEYP_4");         break;
            case 0x5D: mapped = KEYP_5;         sprintf(special, "KEYP_5");         break;
            case 0x5E: mapped = KEYP_6;         sprintf(special, "KEYP_6");         break;
            case 0x5F: mapped = KEYP_7;         sprintf(special, "KEYP_7");         break;
            case 0x60: mapped = KEYP_8;         sprintf(special, "KEYP_8");         break;
            case 0x61: mapped = KEYP_9;         sprintf(special, "KEYP_9");         break;
            case 0x62: mapped = KEYP_0;         sprintf(special, "KEYP_0");         break;

            case 0x63: mapped = KEYP_PERIOD;    sprintf(special, "KEYP_PERIOD");    break;

            default:   mapped = 0;              sprintf(special, "UNMAPPED");       break;
        }
    }

#ifdef HID_DEBUG
    printf("HID -> DOOM: 0x%02x -> 0x%02x %c %s\n", hid_code, mapped, mapped, special);
#endif

    return mapped;
}

static void add_key_to_queue(int pressed, unsigned char key)
{
    if (key == 0) return;

    keyEvent event = {
        .key = key,
        .pressed = !!pressed,
    };

    // Simple overwrite protection (optional, but good practice)
    unsigned int nextWrite = (keyQueueWriteIndex + 1) % KEYQUEUE_SIZE;

    if (nextWrite != keyQueueReadIndex) {
        keyQueue[keyQueueWriteIndex] = event;
        keyQueueWriteIndex = nextWrite;
    }
}

int read_hid_report_queue() {
    unsigned char raw_buf[64] = {0};
    unsigned char *report = NULL;
    int read_bytes_count = 0;

    if (hid_fd < 0) return 0;

    while (true) {
        read_bytes_count = read(hid_fd, raw_buf, sizeof(raw_buf));

        if (read_bytes_count < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return 0;
        }
        if (read_bytes_count == 0) break;

        if (read_bytes_count == 8) report = raw_buf;
        else if (read_bytes_count >= 9) report = raw_buf + 1;
        else continue;

        // --- Revised Modifiers ---
        // HID Modifiers are bits 0-7 in report[0]:
        // 0: LCtrl, 1: LShift, 2: LAlt, 3: LGUI, 4: RCtrl, 5: RShift, 6: RAlt, 7: RGUI
        unsigned char mod_diff = latest_report[0] ^ report[0];
        if (mod_diff) {
            for (int bit = 0; bit < 8; bit++) {
                if (mod_diff & (1 << bit)) {
                    int pressed = !!(report[0] & (1 << bit));
                    // HID Modifier codes start at 0xE0
                    unsigned char hid_mod_code = 0xE0 + bit;
                    add_key_to_queue(pressed, hid_to_doom(hid_mod_code));
                }
            }
        }

        // Keys (Pressed)
        for (int i = 2; i < 8; i++) {
            unsigned char code = report[i];
            if (code <= 3) continue; // Skip no-key/error codes
            int held = 0;
            for (int j = 2; j < 8; j++) if (latest_report[j] == code) held = 1;
            if (!held) add_key_to_queue(1, hid_to_doom(code));
        }

        // Keys (Released)
        for (int i = 2; i < 8; i++) {
            unsigned char code = latest_report[i];
            if (code <= 3) continue;
            int still_held = 0;
            for (int j = 2; j < 8; j++) if (report[j] == code) still_held = 1;
            if (!still_held) add_key_to_queue(0, hid_to_doom(code));
        }

        memcpy(latest_report, report, 8); // Only copy the 8 byte report
    }
    return 0;
}


/* ─────────────────────────────────────────────────────────────────────────
   doomgeneric interface
   ───────────────────────────────────────────────────────────────────────── */

void DG_DrawFrame(void)
{
    // draw frame
    uint32_t* const src = (uint32_t*)DG_ScreenBuffer;
    const int src_w = DOOMGENERIC_RESX;
    const int src_h = DOOMGENERIC_RESY;
    const int dst_w = vinfo.xres;
    const int dst_h = vinfo.yres;
    const int fb_stride = finfo.line_length / sizeof(uint32_t);

    // Use fixed-point math for efficient scaling.
    // The 16 extra bits of precision avoid floating point math.
    const uint32_t y_step = ((uint32_t)src_h << 16) / (uint32_t)dst_h;
    const uint32_t x_step = ((uint32_t)src_w << 16) / (uint32_t)dst_w;

    // Start from the last line of the source buffer for 180-degree rotation.
    uint32_t src_y_fixed = ((uint32_t)(dst_h - 1) * y_step) + (y_step / 2);

    for (int dst_y = 0; dst_y < dst_h; dst_y++)
    {
        uint32_t* src_line = src + ((src_y_fixed >> 16) * src_w);
        uint32_t* dst_line = fb_mem + (dst_y * fb_stride);

        // Start from the last column of the source line for 180-degree rotation.
        uint32_t src_x_fixed = ((uint32_t)(dst_w - 1) * x_step) + (x_step / 2);

        for (int dst_x = 0; dst_x < dst_w; dst_x++)
        {
            dst_line[dst_x] = src_line[src_x_fixed >> 16];
            src_x_fixed -= x_step;
        }
        src_y_fixed -= y_step;
    }
}

int DG_GetKey(int* pressed, unsigned char* doomKey)
{
    read_hid_report_queue();

    if (keyQueueReadIndex == keyQueueWriteIndex)
    {
        return 0;
    }

    keyEvent event = keyQueue[keyQueueReadIndex];
    keyQueueReadIndex++;
    keyQueueReadIndex %= KEYQUEUE_SIZE;

    *pressed = event.pressed;
    *doomKey = event.key;

//#ifdef HID_DEBUG
//    printf("Key: %c %02x %c\n", event.pressed > 0 ? '_' : '@', *doomKey, event.key & 0xFF);
//#endif
    return 1;
}

void DG_Init(void)
{
    /* ── Force unbuffered output so printf works instantly ── */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* ── Open framebuffer ── */
    int fb_fd = open(framebuffer_dev_path, O_RDWR);
    if (fb_fd < 0) {
        I_Error("cannot open framebuffer device file: %s - %s\n", framebuffer_dev_path, strerror(errno));
    }

    // Get screen info
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) < 0)
    {
        I_Error("cannot ioctl framebuffer: %s\n", strerror(errno));
    }

    fb_mem = mmap(NULL, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) {
        I_Error("cannot memory map framebuffer: %s\n", strerror(errno));
    }

    /* ── Open keyboard ── */
    const char *hid_dev = getenv("DOOM_KBDEV");
    if (!hid_dev) {
        hid_dev = default_hid_dev_path;
    }

    hid_fd = open(hid_dev, O_RDONLY | O_NONBLOCK);
    if (hid_fd < 0) {
        I_Error("cannot open HID device %s: %s\n", hid_dev, strerror(errno));
    }

    gettimeofday(&start_time, NULL);
}

void DG_SleepMs(uint32_t ms)
{
    usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return (uint32_t)((now.tv_sec  - start_time.tv_sec)  * 1000 +
                      (now.tv_usec - start_time.tv_usec) / 1000);
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

/* ─────────────────────────────────────────────────────────────────────────
   Entrypoint
   ───────────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    doomgeneric_Create(argc, argv);
    while (true) {
        doomgeneric_Tick();
    }
}
