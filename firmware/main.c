/*
 * Clawd Usage Monitor — ESP-12F firmware
 *
 * Polls a local HTTP proxy for Claude Code subscription usage and
 * renders the result on a 1.54" 240x240 ST7789 LCD via HSPI.
 *
 * Hardware
 *   SCLK = GPIO14 (HSPICLK / MTMS)
 *   MOSI = GPIO13 (HSPID  / MTCK)
 *   CS   = GPIO15 (HSPICS / MTDO)
 *   DC   = GPIO2  (swap with RST if display stays blank)
 *   RST  = GPIO4
 *
 * The screen is split into three regions, all inside an 8px safe zone:
 *   - Header   : "Pro" + clock
 *   - Clawd    : 20x20 sprite scaled 5x (= 100x100), centered
 *   - Panel 1  : Current session bar + %/reset
 *   - Panel 2  : Weekly limits bar + %/reset
 */

#include "user_config.h"

#include "c_types.h"
#include "ets_sys.h"
#include "osapi.h"
#include "user_interface.h"
#include "espconn.h"
#include "mem.h"
#include "gpio.h"
#include "eagle_soc.h"
#include "pgmspace.h"

#include "src/splash_animations.h"
#include "src/font_data.h"
#include "json/jsonparse.h"
#include "sntp.h"

/* =========================================================================
 * Layout / colors
 * ========================================================================= */
#define SCREEN_W      240
#define SCREEN_H      240
#define SAFE          8

/* RGB565 — Claude Code theme palette (warm dark + Claude orange accent).
 *   BG     #1A1612  warm dark brown (matches Claude dark UI)
 *   ACCENT #D97757  "Claude orange" — used for header/clock/highlights
 *   TEXT   #E5DDD5  cream / off-white
 *   MUTED  #8A7E70  warm gray for secondary info
 *   BAR_BG #2D2620  slightly lighter than BG
 */
#define COL_BG        0x18A2
#define COL_HEADER    0xDBAA   /* Claude orange */
#define COL_TEXT      0xE6FB   /* cream */
#define COL_CLOCK     0xDBAA   /* Claude orange */
#define COL_MUTED     0x8BCE   /* warm gray */
#define COL_BAR_BG    0x2924   /* dim warm */
#define COL_GREEN     0x9759
#define COL_YELLOW    0xFAF5
#define COL_ORANGE    0xDBAA   /* Claude orange for warning bars */
#define COL_RED       0xE24A
#define COL_PANEL     0x18A2
#define COL_BORDER    0x2D28

/* Layout (absolute pixel coords). Total must fit within 240×240.
 * Vertical budget:
 *   header   24  (y=8..32)
 *   gap       0
 *   clawd    80  (y=32..112) — scale 4× (was 5×)
 *   gap       6
 *   panel1   62  (y=118..180): 24 label + 2 + 10 bar + 2 + 24 status
 *   gap       0
 *   panel2   62  (y=178..240): same layout
 *   total   240  ✓
 */
#define HEADER_Y      8
#define CLAWD_SCALE   4
#define CLAWD_SIZE    (20 * CLAWD_SCALE)    /* 80 */
#define CLAWD_X       ((SCREEN_W - CLAWD_SIZE) / 2)   /* 80 */
#define CLAWD_Y       32
#define PANEL1_Y      118
#define PANEL2_Y      178
#define BAR_H         10
#define FONT_W        16
#define FONT_H        24
#define TEXT_SCALE    1
/* Each 16x24 glyph only uses rows 0–15 (caps) or 4–15 (lowercase+descender);
 * the bottom 8 rows of every canvas are empty. We exploit that by pulling
 * the bar UP into the label's empty rows (BAR_PAD_TOP = -6) and giving
 * the status line more breathing room below the bar (STATUS_GAP = 8). */
#define BAR_PAD_TOP   (-6)           /* bar overlaps empty bottom of label */
#define STATUS_GAP    8              /* visual gap to '%' digits          */

/* =========================================================================
 * Pins
 * ========================================================================= */
#define PIN_DC   0
#define PIN_RST  2

/* GPIO helpers — written through the SDK macros so we don't reinvent them. */
#define DC_SET(v)  GPIO_OUTPUT_SET(PIN_DC, (v))
#define RST_SET(v) GPIO_OUTPUT_SET(PIN_RST, (v))

/* =========================================================================
 * BIT-BANG SPI DRIVER — bypass HSPI peripheral entirely.
 *
 * HSPI registers show correct setup + TRANS_DONE=1 but data does not
 * reach the LCD.  Switch to bit-banging on GPIO13 (MOSI) and GPIO14 (SCLK)
 * via direct GPIO register writes (GPOS/GPOC) — same approach TFT_eSPI
 * uses for DC/CS toggling.
 *
 * SPI Mode 3 (CPOL=1, CPHA=1) timing:
 *   - SCK idles HIGH between transactions
 *   - For each bit (MSB first):
 *       1. Drive SCK LOW (falling edge — slave does NOT sample)
 *       2. Set MOSI to bit value (must be stable before next rising edge)
 *       3. Drive SCK HIGH (rising edge — slave samples MOSI)
 * ========================================================================= */

/* GPIO output set/clear registers (single-cycle writes). */
#define GPIO_OUT_W1TS_REG (*(volatile uint32_t*)0x60000304)  /* set bits HIGH */
#define GPIO_OUT_W1TC_REG (*(volatile uint32_t*)0x60000308)  /* clear bits to LOW */

#define BB_MOSI_BIT (1u << 13)  /* GPIO13 = MOSI */
#define BB_SCLK_BIT (1u << 14)  /* GPIO14 = SCLK */

#define BB_MOSI_HIGH() (GPIO_OUT_W1TS_REG = BB_MOSI_BIT)
#define BB_MOSI_LOW()  (GPIO_OUT_W1TC_REG = BB_MOSI_BIT)
#define BB_SCLK_HIGH() (GPIO_OUT_W1TS_REG = BB_SCLK_BIT)
#define BB_SCLK_LOW()  (GPIO_OUT_W1TC_REG = BB_SCLK_BIT)

static void ICACHE_FLASH_ATTR spi_write_byte(uint8_t b) {
    /* Send 8 bits MSB-first using SPI Mode 3 timing */
    int i;
    for (i = 7; i >= 0; i--) {
        BB_SCLK_LOW();                       /* falling edge — change MOSI */
        if ((b >> i) & 1) BB_MOSI_HIGH();
        else              BB_MOSI_LOW();
        BB_SCLK_HIGH();                      /* rising edge — slave samples MOSI */
    }
}

static void ICACHE_FLASH_ATTR spi_write_bytes(const uint8_t *data, uint16_t n) {
    uint16_t i;
    for (i = 0; i < n; i++) {
        spi_write_byte(data[i]);
    }
}

static void ICACHE_FLASH_ATTR spi_write_color_repeat(uint16_t color, uint32_t n) {
    uint8_t hi = (color >> 8) & 0xFF;
    uint8_t lo = color & 0xFF;
    while (n > 0) {
        spi_write_byte(hi);
        spi_write_byte(lo);
        n--;
    }
}

static void ICACHE_FLASH_ATTR hspi_init(void) {
    /* Mux GPIO13/14 as GENERAL-PURPOSE GPIO (function 3), NOT HSPI.
     * Then drive them manually via GPO Set/Clear registers. */
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTCK_U, FUNC_GPIO13); /* GPIO13 = GPIO (MOSI) */
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTMS_U, FUNC_GPIO14); /* GPIO14 = GPIO (SCLK) */

    /* Both pins as output. SCK idles HIGH (Mode 3); MOSI any state. */
    GPIO_OUTPUT_SET(13, 0);                 /* MOSI low (initial) */
    GPIO_OUTPUT_SET(14, 1);                 /* SCLK HIGH (idle for Mode 3) */
}

/* Stub for backward compatibility — bit-bang has no registers to dump */
static void ICACHE_FLASH_ATTR spi_dump_regs(const char *tag) {
    os_printf("[spi] bit-bang SPI active (no registers); tag=%s\n", tag);
}

/* =========================================================================
 * ST7789 driver
 * ========================================================================= */
static void ICACHE_FLASH_ATTR st_cmd(uint8_t c) {
    DC_SET(0);
    spi_write_byte(c);
}

static void ICACHE_FLASH_ATTR st_data(uint8_t d) {
    DC_SET(1);
    spi_write_byte(d);
}

static void ICACHE_FLASH_ATTR st_set_window(uint16_t x0, uint16_t y0,
                                            uint16_t x1, uint16_t y1) {
    st_cmd(0x2A);                  /* CASET */
    DC_SET(1);
    uint8_t b[4] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    spi_write_bytes(b, 4);

    st_cmd(0x2B);                  /* RASET */
    DC_SET(1);
    b[0] = y0 >> 8; b[1] = y0 & 0xFF; b[2] = y1 >> 8; b[3] = y1 & 0xFF;
    spi_write_bytes(b, 4);

    st_cmd(0x2C);                  /* RAMWR — caller streams pixel data */
    DC_SET(1);
}

static void ICACHE_FLASH_ATTR st_fill_rect(int16_t x, int16_t y,
                                           int16_t w, int16_t h,
                                           uint16_t color) {
    if (w <= 0 || h <= 0) return;
    if (x >= SCREEN_W || y >= SCREEN_H) return;
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    st_set_window(x, y, x + w - 1, y + h - 1);
    spi_write_color_repeat(color, (uint32_t)w * (uint32_t)h);
}

static void ICACHE_FLASH_ATTR st_fill(uint16_t color) {
    st_fill_rect(0, 0, SCREEN_W, SCREEN_H, color);
}

static void ICACHE_FLASH_ATTR st_draw_pixel(int16_t x, int16_t y, uint16_t color) {
    if (x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) return;
    st_set_window(x, y, x, y);
    uint8_t b[2] = { color >> 8, color & 0xFF };
    spi_write_bytes(b, 2);
}

static void ICACHE_FLASH_ATTR st7789_init(void) {
    /* GPIO setup for DC (GPIO0) / RST (GPIO2) / BL (GPIO5). */
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_GPIO0);   /* DC  */
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_GPIO2);   /* RST */
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO5_U, FUNC_GPIO5);   /* BL  */
    GPIO_OUTPUT_SET(PIN_DC, 1);
    GPIO_OUTPUT_SET(PIN_RST, 1);
    GPIO_OUTPUT_SET(5, 0);  /* backlight ON (active-low on SmallTV Ultra) */

    /* Hardware reset pulse. */
    RST_SET(1); os_delay_us(10000);
    RST_SET(0); os_delay_us(10000);
    RST_SET(1); os_delay_us(150000);

    st_cmd(0x01); os_delay_us(150000);          /* SWRESET                  */
    st_cmd(0x11); os_delay_us(120000);          /* SLPOUT                   */

    st_cmd(0x3A); st_data(0x55);                /* COLMOD: 16-bit/pixel     */
    st_cmd(0x36); st_data(0x00);                /* MADCTL                   */
    st_cmd(0x21);                               /* INVON  — ST7789 needs it */

    /* Address windows for a 240x240 IPS. */
    st_cmd(0x2A); DC_SET(1);
    { uint8_t b[4] = {0,0,0,239}; spi_write_bytes(b, 4); }
    st_cmd(0x2B); DC_SET(1);
    { uint8_t b[4] = {0,0,0,239}; spi_write_bytes(b, 4); }

    st_cmd(0x13);                               /* Normal display mode      */
    st_cmd(0x29); os_delay_us(10000);           /* DISPON                   */
}

/* =========================================================================
 * Font renderer  —  font_data.h provides one PROGMEM uint16_t[16*24]
 *                   per glyph; 0xFFFF = ink, 0x0000 = transparent.
 *
 * NOTE: the strings used by the UI ("waiting", "LIMIT REACHED",
 * "Weekly", "Xh Xm", ...) reference glyphs that are NOT yet present
 * in font_data.h.  Add the missing characters before flashing — at minimum:
 *     m  w  f  g  L  I  R  E  A  H  D
 * Without them, those strings render as blanks (draw_char becomes a noop).
 * ========================================================================= */
static const uint16_t* ICACHE_FLASH_ATTR font_for_char(char c) {
    switch (c) {
        /* digits */
        case '0': return font_0; case '1': return font_1;
        case '2': return font_2; case '3': return font_3;
        case '4': return font_4; case '5': return font_5;
        case '6': return font_6; case '7': return font_7;
        case '8': return font_8; case '9': return font_9;
        /* punctuation */
        case '%': return font_percent;
        case ':': return font_colon;
        case '/': return font_slash;
        case ' ': return font_space;
        /* uppercase */
        case 'A': return font_A; case 'C': return font_C;
        case 'D': return font_D; case 'E': return font_E;
        case 'H': return font_H; case 'I': return font_I;
        case 'L': return font_L; case 'M': return font_M;
        case 'P': return font_P; case 'R': return font_R;
        case 'S': return font_S; case 'T': return font_T;
        case 'W': return font_W;
        /* lowercase */
        case 'a': return font_a; case 'd': return font_d;
        case 'e': return font_e; case 'f': return font_f;
        case 'g': return font_g; case 'h': return font_h;
        case 'i': return font_i; case 'k': return font_k;
        case 'l': return font_l; case 'm': return font_m;
        case 'n': return font_n; case 'o': return font_o;
        case 'r': return font_r; case 's': return font_s;
        case 't': return font_t; case 'u': return font_u;
        case 'w': return font_w; case 'x': return font_x;
        case 'y': return font_y;
        default:  return NULL;
    }
}

/* Draw one 16x24 glyph at (x,y) in `color` over transparent pixels.
 * The font is a 1-bit mask stored as RGB565 (0xFFFF = ink). */
static void ICACHE_FLASH_ATTR draw_char(int16_t x, int16_t y, char c, uint16_t color) {
    const uint16_t *g = font_for_char(c);
    if (g == NULL) return;

    int16_t cx, cy;
    for (cy = 0; cy < FONT_H; cy++) {
        for (cx = 0; cx < FONT_W; cx++) {
            uint16_t v = pgm_read_word(&g[cy * FONT_W + cx]);
            if (v != 0) {
                st_draw_pixel(x + cx, y + cy, color);
            }
        }
    }
}

static void ICACHE_FLASH_ATTR draw_text(int16_t x, int16_t y,
                                        const char *s, uint16_t color) {
    while (*s) {
        draw_char(x, y, *s, color);
        x += FONT_W;
        s++;
    }
}

static int16_t ICACHE_FLASH_ATTR text_width(const char *s) {
    int16_t n = 0;
    while (*s++) n++;
    return n * FONT_W;
}

/* =========================================================================
 * Clawd renderer  —  reads splash frames from flash via pgm_read_byte
 * ========================================================================= */
static uint16_t cur_palette[10];           /* RAM-cached palette (20 bytes) */
static uint8_t  cur_frame_buf[400];        /* RAM-cached frame  (400 bytes) */

static void ICACHE_FLASH_ATTR cache_anim_frame(const splash_anim_def_t *anim,
                                               uint16_t frame_idx) {
    uint8_t i;
    for (i = 0; i < 10; i++) {
        cur_palette[i] = pgm_read_word(&anim->palette[i]);
    }
    uint16_t j;
    const uint8_t *src = anim->frames[frame_idx];
    for (j = 0; j < 400; j++) {
        cur_frame_buf[j] = pgm_read_byte(&src[j]);
    }
}

/* Stream a 100x100 windowed pixel write at (CLAWD_X, CLAWD_Y).
 * For transparent pixels (palette index 0) we emit COL_BG instead so the
 * previous frame is erased without a separate clear pass. */
/* One sprite row → CLAWD_SIZE pixels × 2 bytes (RGB565). All values derived
 * from CLAWD_SCALE so changing the scale doesn't break this routine. */
#define CLAWD_SCANLINE_BYTES (CLAWD_SIZE * 2)

static void ICACHE_FLASH_ATTR render_clawd(void) {
    st_set_window(CLAWD_X, CLAWD_Y,
                  CLAWD_X + CLAWD_SIZE - 1,
                  CLAWD_Y + CLAWD_SIZE - 1);

    /* Build one full destination scanline in RAM, stream it CLAWD_SCALE times
     * per sprite row (vertical replication). With bit-bang SPI there's no
     * FIFO limit so we can push the whole scanline in one call. */
    static uint8_t scanline[CLAWD_SCANLINE_BYTES];

    uint8_t sy, px, py, k;
    for (py = 0; py < 20; py++) {
        /* Fill scanline: each source pixel expands to CLAWD_SCALE dst pixels. */
        for (px = 0; px < 20; px++) {
            uint8_t idx = cur_frame_buf[py * 20 + px];
            uint16_t color = (idx == 0) ? COL_BG : cur_palette[idx];
            uint8_t hi = color >> 8, lo = color & 0xFF;
            uint16_t dst = px * CLAWD_SCALE;
            for (k = 0; k < CLAWD_SCALE; k++) {
                scanline[(dst + k) * 2]     = hi;
                scanline[(dst + k) * 2 + 1] = lo;
            }
        }
        /* Repeat scanline CLAWD_SCALE times for vertical replication. */
        for (sy = 0; sy < CLAWD_SCALE; sy++) {
            spi_write_bytes(scanline, CLAWD_SCANLINE_BYTES);
        }
    }
}

/* =========================================================================
 * Format helpers
 * ========================================================================= */
/* Append integer + suffix to buf; returns new write position. */
static int ICACHE_FLASH_ATTR write_int(char *buf, int pos, int v) {
    if (v < 0) v = 0;
    if (v == 0) { buf[pos++] = '0'; return pos; }
    char tmp[12];
    int n = 0;
    while (v > 0 && n < 11) { tmp[n++] = '0' + (v % 10); v /= 10; }
    while (n > 0)           { buf[pos++] = tmp[--n]; }
    return pos;
}

/* Session reset:  "Xh Xm" | "Xm" | "waiting" (when seconds <= 0). */
static void ICACHE_FLASH_ATTR fmt_session_time(int secs, char *out) {
    int pos = 0;
    if (secs <= 0) {
        const char *s = "waiting";
        while (*s) out[pos++] = *s++;
        out[pos] = 0;
        return;
    }
    /* prefix "resets in " */
    const char *p = "in ";
    while (*p) out[pos++] = *p++;

    int mins  = secs / 60;
    int hours = mins / 60;
    int rem_m = mins % 60;
    if (hours > 0) {
        pos = write_int(out, pos, hours); out[pos++] = 'h'; out[pos++] = ' ';
        pos = write_int(out, pos, rem_m); out[pos++] = 'm';
    } else {
        pos = write_int(out, pos, mins);  out[pos++] = 'm';
    }
    out[pos] = 0;
}

/* Weekly reset:  "Xd Yh" | "Xh Ym" | "Xm" — same single-digit-spacing rule. */
static void ICACHE_FLASH_ATTR fmt_weekly_time(int secs, char *out) {
    int pos = 0;
    const char *p = "in ";
    while (*p) out[pos++] = *p++;

    if (secs < 0) secs = 0;
    int mins  = secs / 60;
    int hours = mins / 60;
    int days  = hours / 24;
    int rem_h = hours % 24;
    int rem_m = mins % 60;

    if (days > 0) {
        pos = write_int(out, pos, days);  out[pos++] = 'd'; out[pos++] = ' ';
        pos = write_int(out, pos, rem_h); out[pos++] = 'h';
    } else if (hours > 0) {
        pos = write_int(out, pos, hours); out[pos++] = 'h'; out[pos++] = ' ';
        pos = write_int(out, pos, rem_m); out[pos++] = 'm';
    } else {
        pos = write_int(out, pos, mins);  out[pos++] = 'm';
    }
    out[pos] = 0;
}

/* Fill buf with "HH:MM" (UTC+7).  Returns empty string until SNTP syncs
 * (font doesn't have '-' glyph, so "--:--" would render as partial colons). */
#define TZ_OFFSET_SECS  (7 * 3600)
static void ICACHE_FLASH_ATTR get_time_str(char *buf) {
    uint32 ts = sntp_get_current_timestamp();
    if (ts == 0) {
        buf[0] = 0;  /* empty — skip drawing clock until time is known */
        return;
    }
    ts += TZ_OFFSET_SECS;
    int h = (int)((ts / 3600) % 24);
    int m = (int)((ts /   60) % 60);
    buf[0] = '0' + (h / 10);
    buf[1] = '0' + (h % 10);
    buf[2] = ':';
    buf[3] = '0' + (m / 10);
    buf[4] = '0' + (m % 10);
    buf[5] = 0;
}

static uint16_t ICACHE_FLASH_ATTR bar_color(int pct) {
    if (pct >= 100) return COL_RED;
    if (pct >=  90) return COL_ORANGE;
    if (pct >=  70) return COL_YELLOW;
    return COL_GREEN;
}

/* =========================================================================
 * Animation selection + state
 * ========================================================================= */
typedef enum {
    ANIM_IDLE_BREATHE = 0,
    ANIM_IDLE_BLINK,
    ANIM_IDLE_LOOK_AROUND,
    ANIM_EXPRESSION_WINK,
    ANIM_EXPRESSION_SURPRISE,
    ANIM_EXPRESSION_SLEEP,
    ANIM_DANCE_BOUNCE,
    ANIM_DANCE_SWAY,
    ANIM_WORK_CODING,
    ANIM_WORK_THINK,
    ANIM_DANCE_BOUNCE_DJ,
    ANIM_DANCE_SWAY_DJ,
    ANIM_DANCE_DJMIX,
} anim_id_t;

static int ICACHE_FLASH_ATTR pick_animation(int session_pct) {
    if (session_pct >= 100) return ANIM_EXPRESSION_SLEEP;
    if (session_pct >=  90) return ANIM_EXPRESSION_SURPRISE;
    if (session_pct >=  70) return ANIM_WORK_CODING;
    if (session_pct >=  40) return ANIM_WORK_THINK;
    return ANIM_IDLE_BREATHE;
}

/* Live state, updated every poll / every animation tick. */
static int       g_session_pct   = 0;
static int       g_session_reset = 0;
static int       g_weekly_pct    = 0;
static int       g_weekly_reset  = 0;

static int       g_current_anim  = ANIM_IDLE_BREATHE;
static uint16_t  g_current_frame = 0;
static os_timer_t anim_timer;

/* =========================================================================
 * UI render — full screen redraw + Clawd-only redraw for animation ticks
 * ========================================================================= */
static void ICACHE_FLASH_ATTR render_panel(int yTop, const char *label,
                                           int pct, int reset_secs,
                                           int is_session) {
    /* clear the panel region first so old text doesn't bleed.
     * Height = label + bar_pad + bar + status_gap + status = 62px exactly. */
    st_fill_rect(SAFE, yTop, SCREEN_W - 2 * SAFE,
                 FONT_H + BAR_PAD_TOP + BAR_H + STATUS_GAP + FONT_H, COL_BG);

    draw_text(SAFE, yTop, label, COL_TEXT);

    /* === ALWAYS draw the bar (even when idle) === */
    int bar_y = yTop + FONT_H + BAR_PAD_TOP;
    int bar_w = SCREEN_W - 2 * SAFE;
    st_fill_rect(SAFE, bar_y, bar_w, BAR_H, COL_BAR_BG);
    int fill_w = (bar_w * pct) / 100;
    if (fill_w > bar_w) fill_w = bar_w;
    if (fill_w > 0) {
        st_fill_rect(SAFE, bar_y, fill_w, BAR_H, bar_color(pct));
    }

    /* === Status line === */
    int status_y = bar_y + BAR_H + STATUS_GAP;

    if (is_session && reset_secs == 0 && pct == 0) {
        /* idle — show "waiting" right-aligned */
        const char *idle = "waiting";
        int16_t iw = text_width(idle);
        draw_text(SCREEN_W - SAFE - iw, status_y, idle, COL_MUTED);
    } else if (pct >= 100) {
        /* limit reached */
        draw_text(SAFE, status_y, "LIMIT REACHED", COL_RED);
    } else {
        /* "NN%" left */
        char buf[8];
        int n = 0;
        n = write_int(buf, n, pct); buf[n++] = '%'; buf[n] = 0;
        draw_text(SAFE, status_y, buf, COL_TEXT);

        /* reset string right-aligned */
        char rbuf[40];
        if (is_session) fmt_session_time(reset_secs, rbuf);
        else            fmt_weekly_time(reset_secs, rbuf);
        int16_t rw = text_width(rbuf);
        int16_t rx = SCREEN_W - SAFE - rw;
        if (rx < SAFE) rx = SAFE;
        draw_text(rx, status_y, rbuf, COL_MUTED);
    }
}

static void ICACHE_FLASH_ATTR render_screen(void) {
    /* full background */
    st_fill(COL_BG);

    /* header */
    draw_text(SAFE, HEADER_Y, "Pro", COL_TEXT);
    /* clock — from SNTP (UTC+7); shows "--:--" until first sync */
    char clock_buf[6];
    get_time_str(clock_buf);
    int16_t cw = text_width(clock_buf);
    draw_text(SCREEN_W - SAFE - cw, HEADER_Y, clock_buf, COL_CLOCK);

    /* Clawd */
    render_clawd();

    /* panels */
    render_panel(PANEL1_Y, "Current",
                 g_session_pct, g_session_reset, 1);
    render_panel(PANEL2_Y, "Weekly",
                 g_weekly_pct, g_weekly_reset, 0);
}

/* =========================================================================
 * Animation timer  —  re-arms with the next frame's hold time.
 * Redraws only the Clawd region (NOT the entire screen) to keep it fast.
 * ========================================================================= */
static void ICACHE_FLASH_ATTR anim_timer_cb(void *arg);

static void ICACHE_FLASH_ATTR schedule_next_frame(void) {
    const splash_anim_def_t *a = &splash_anims[g_current_anim];
    uint16_t hold_ms = pgm_read_word(&a->holds[g_current_frame]);
    if (hold_ms < 30) hold_ms = 30;
    os_timer_disarm(&anim_timer);
    os_timer_setfn(&anim_timer, anim_timer_cb, NULL);
    os_timer_arm(&anim_timer, hold_ms, 0);   /* one-shot */
}

static void ICACHE_FLASH_ATTR anim_timer_cb(void *arg) {
    const splash_anim_def_t *a = &splash_anims[g_current_anim];
    g_current_frame = (g_current_frame + 1) % a->frame_count;
    cache_anim_frame(a, g_current_frame);
    render_clawd();
    schedule_next_frame();
}

static void ICACHE_FLASH_ATTR maybe_switch_animation(void) {
    int new_id = pick_animation(g_session_pct);
    if (new_id == g_current_anim) return;
    g_current_anim = new_id;
    g_current_frame = 0;
    cache_anim_frame(&splash_anims[new_id], 0);
    render_clawd();
    schedule_next_frame();
}

/* =========================================================================
 * JSON parsing — minimal hand-rolled parser since the SDK's libjson.a was
 * compiled without JSON_FORMAT defined (all jsonparse_* functions are
 * empty stubs). The proxy response is a fixed flat object, so we just
 * locate each key with strstr and read the integer value that follows.
 *   body = {"ok":true,"session_pct":37,"session_reset_in":15835, ... }
 * ========================================================================= */
static int ICACHE_FLASH_ATTR find_int_after(const char *body, const char *key, int *out) {
    const char *p = (const char *)os_strstr(body, key);
    if (!p) return 0;
    p += os_strlen(key);
    /* Skip past ":" and any whitespace / quote marks */
    while (*p == ':' || *p == ' ' || *p == '"') p++;
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    *out = v;
    return 1;
}

static int ICACHE_FLASH_ATTR parse_usage(const char *body, int len) {
    (void)len;       /* body is NUL-terminated by http_recv_cb */
    int got = 0;
    int v;
    if (find_int_after(body, "\"session_pct\"",      &v)) { g_session_pct   = v; got++; }
    if (find_int_after(body, "\"session_reset_in\"", &v)) { g_session_reset = v; got++; }
    if (find_int_after(body, "\"weekly_pct\"",       &v)) { g_weekly_pct    = v; got++; }
    if (find_int_after(body, "\"weekly_reset_in\"",  &v)) { g_weekly_reset  = v; got++; }
    return got;
}

/* =========================================================================
 * HTTP GET  —  espconn TCP client.  Pipeline: resolve -> connect -> send
 *              GET -> recv response -> parse JSON -> close -> rerender.
 * ========================================================================= */
static struct espconn http_conn;
static esp_tcp        http_tcp;
static ip_addr_t      http_ip;

static char http_recv[1024];
static uint16_t http_recv_len = 0;

static void ICACHE_FLASH_ATTR http_recv_cb(void *arg, char *data, unsigned short len) {
    if (http_recv_len + len < sizeof(http_recv) - 1) {
        os_memcpy(http_recv + http_recv_len, data, len);
        http_recv_len += len;
        http_recv[http_recv_len] = 0;
    }
}

static void ICACHE_FLASH_ATTR http_disconnect_cb(void *arg) {
    /* find blank line between headers and body */
    const char *body = NULL;
    int i;
    for (i = 0; i + 3 < (int)http_recv_len; i++) {
        if (http_recv[i] == '\r' && http_recv[i+1] == '\n' &&
            http_recv[i+2] == '\r' && http_recv[i+3] == '\n') {
            body = http_recv + i + 4;
            break;
        }
    }
    if (body) {
        int blen = http_recv_len - (body - http_recv);
        int got = parse_usage(body, blen);
        os_printf("[http] ok got=%d s=%d w=%d\n", got, g_session_pct, g_weekly_pct);
        maybe_switch_animation();
        render_screen();
    } else {
        os_printf("[http] no body\n");
    }
    http_recv_len = 0;
}

static void ICACHE_FLASH_ATTR http_connect_cb(void *arg) {
    struct espconn *c = (struct espconn *)arg;
    espconn_regist_recvcb(c, http_recv_cb);
    espconn_regist_disconcb(c, http_disconnect_cb);

    static char req[200];
    int n = os_sprintf(req,
        "GET /usage HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Connection: close\r\n"
        "User-Agent: clawd-monitor/1\r\n"
        "\r\n",
        PROXY_HOST, PROXY_PORT);
    espconn_sent(c, (uint8 *)req, n);
}

static void ICACHE_FLASH_ATTR http_recon_cb(void *arg, sint8 err) {
    os_printf("[http] err=%d\n", err);
    http_recv_len = 0;
}

static void ICACHE_FLASH_ATTR http_dns_cb(const char *name, ip_addr_t *ipaddr, void *arg) {
    struct espconn *c = (struct espconn *)arg;
    if (ipaddr == NULL) return;
    os_memcpy(c->proto.tcp->remote_ip, &ipaddr->addr, 4);
    espconn_regist_connectcb(c, http_connect_cb);
    espconn_regist_reconcb(c, http_recon_cb);
    espconn_connect(c);
}

static void ICACHE_FLASH_ATTR http_get_usage(void) {
    http_recv_len = 0;

    os_memset(&http_conn, 0, sizeof(http_conn));
    os_memset(&http_tcp,  0, sizeof(http_tcp));
    http_conn.type = ESPCONN_TCP;
    http_conn.state = ESPCONN_NONE;
    http_conn.proto.tcp = &http_tcp;
    http_tcp.local_port  = espconn_port();
    http_tcp.remote_port = PROXY_PORT;

    /* The proxy host can be either a dotted-quad or a DNS name; gethostbyname
     * handles both. */
    sint8 r = espconn_gethostbyname(&http_conn, PROXY_HOST, &http_ip, http_dns_cb);
    if (r == ESPCONN_OK) {
        /* already in cache — proceed immediately */
        http_dns_cb(PROXY_HOST, &http_ip, &http_conn);
    }
    /* r == ESPCONN_INPROGRESS — dns cb will fire when resolution completes  */
}

/* =========================================================================
 * Poll timer  —  fires every POLL_INTERVAL seconds.
 * ========================================================================= */
static os_timer_t poll_timer;

static void ICACHE_FLASH_ATTR poll_timer_cb(void *arg) {
    http_get_usage();
}

/* =========================================================================
 * Clock timer — updates only the header (top row) every 15s.
 * Runs independently of HTTP polling so the clock works without proxy.
 * ========================================================================= */
static os_timer_t clock_timer;

static void ICACHE_FLASH_ATTR redraw_header(void) {
    /* Clear the header strip first */
    st_fill_rect(0, HEADER_Y, SCREEN_W, FONT_H, COL_BG);

    /* Left: "Pro" in Claude orange */
    draw_text(SAFE, HEADER_Y, "Pro", COL_HEADER);

    /* Right: clock — only drawn if SNTP has synced */
    char clock_buf[6];
    get_time_str(clock_buf);
    if (clock_buf[0]) {
        int16_t cw = text_width(clock_buf);
        draw_text(SCREEN_W - SAFE - cw, HEADER_Y, clock_buf, COL_CLOCK);
    }
}

static void ICACHE_FLASH_ATTR clock_timer_cb(void *arg) {
    redraw_header();
}

/* =========================================================================
 * Wi-Fi
 * ========================================================================= */
static void ICACHE_FLASH_ATTR wifi_event_cb(System_Event_t *e) {
    switch (e->event) {
        case EVENT_STAMODE_GOT_IP: {
            os_printf("[wifi] got IP\n");

            /* Start SNTP — SDK v3 defaults to poll mode, no setoperatingmode needed.
             * sntp_set_timezone(0) keeps UTC; we add TZ offset manually. */
            static char ntp_server[] = "pool.ntp.org";
            sntp_set_timezone(0);
            sntp_setservername(0, ntp_server);
            sntp_init();

            /* Clock timer: re-draw header every 10s so SNTP-synced clock appears
             * even if the HTTP proxy is offline.  Repeating from the start
             * — first tick lands ~10s after WiFi GOT_IP, plenty for SNTP sync. */
            os_timer_disarm(&clock_timer);
            os_timer_setfn(&clock_timer, clock_timer_cb, NULL);
            os_timer_arm(&clock_timer, 10000, 1);  /* repeating, every 10s */

            /* one immediate poll, then arm the periodic timer */
            http_get_usage();
            os_timer_disarm(&poll_timer);
            os_timer_setfn(&poll_timer, poll_timer_cb, NULL);
            os_timer_arm(&poll_timer, POLL_INTERVAL * 1000, 1);
            break;
        }
        case EVENT_STAMODE_DISCONNECTED:
            os_printf("[wifi] disconnected\n");
            os_timer_disarm(&poll_timer);
            os_timer_disarm(&clock_timer);
            break;
        default:
            break;
    }
}

static void ICACHE_FLASH_ATTR wifi_init(void) {
    wifi_set_opmode(STATION_MODE);

    struct station_config conf;
    os_memset(&conf, 0, sizeof(conf));
    os_strncpy((char *)conf.ssid,     WIFI_SSID,     sizeof(conf.ssid));
    os_strncpy((char *)conf.password, WIFI_PASSWORD, sizeof(conf.password));
    conf.bssid_set = 0;

    wifi_station_set_config(&conf);
    wifi_set_event_handler_cb(wifi_event_cb);
    wifi_station_connect();
}

/* =========================================================================
 * Init
 * ========================================================================= */
void ICACHE_FLASH_ATTR user_init(void) {
    /* Standard SDK setup. */
    uart_div_modify(0, UART_CLK_FREQ / 115200);
    os_printf("\n[clawd] boot\n");

    /* ---------- GPIO15 boot guard ----------
     * ESP8266 reads GPIO15 (MTDO) at reset to choose the boot mode; it MUST
     * be LOW for flash-boot.  The PCB should hold it down with a 10k
     * pulldown.  If user_init() runs at all the strapping was OK at reset,
     * but the line can still float HIGH afterwards on a board missing the
     * pulldown — and that breaks HSPI_CS once we mux GPIO15 to the SPI
     * peripheral.  Mux GPIO15 as a plain input, read it, and bail to a red
     * screen if it's HIGH so a misbuilt board is obvious.
     */
    PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDO_U, FUNC_GPIO15);
    GPIO_DIS_OUTPUT(15);
    os_delay_us(200);   /* let the line settle after switching mux */
    if (GPIO_INPUT_GET(15)) {
        os_printf("[boot] GPIO15 HIGH — missing pulldown, halting.\n");
        /* Bring up SPI + LCD just enough to paint a red error frame and
         * stop.  CS will be driven by HSPI from here on; if there's a
         * conflicting external pullup the display may still look glitched,
         * but red-on-screen is enough of a signal that the board is bad. */
        hspi_init();
        st7789_init();
        st_fill(COL_RED);
        return;
    }

    /* SPI + LCD */
    hspi_init();
    os_printf("[boot] hspi_init done (bit-bang on GPIO13/14)\n");
    st7789_init();
    os_printf("[boot] st7789_init done\n");

    /* Initial UI */
    st_fill(COL_BG);
    draw_text(SAFE, HEADER_Y, "Pro", COL_TEXT);

    /* Initial Clawd render before Wi-Fi connects. */
    cache_anim_frame(&splash_anims[g_current_anim], 0);
    render_clawd();
    schedule_next_frame();

    /* Idle panels — bars empty, "waiting" for the session panel. */
    render_panel(PANEL1_Y, "Current", 0, 0, 1);
    render_panel(PANEL2_Y, "Weekly",   0, 0, 0);
    os_printf("[boot] initial UI rendered\n");

    /* Wi-Fi connect — DNS/HTTP will follow when GOT_IP fires. */
    wifi_init();
    os_printf("[boot] wifi_init done\n");
}

/* NonOS SDK 3.x partition table — 4MB (32Mbit) flash */
void ICACHE_FLASH_ATTR user_pre_init(void) {
    static const partition_item_t part_table[] = {
        { SYSTEM_PARTITION_BOOTLOADER,        0x000000, 0x1000  },
        { SYSTEM_PARTITION_OTA_1,             0x001000, 0xF1000 },
        { SYSTEM_PARTITION_OTA_2,             0x101000, 0xF1000 },
        { SYSTEM_PARTITION_RF_CAL,            0x3FB000, 0x1000  },
        { SYSTEM_PARTITION_PHY_DATA,          0x3FC000, 0x1000  },
        { SYSTEM_PARTITION_SYSTEM_PARAMETER,  0x3FD000, 0x3000  },
    };
    system_partition_table_regist(
        part_table,
        sizeof(part_table) / sizeof(part_table[0]),
        FLASH_SIZE_32M_MAP_1024_1024);
}
void user_rf_pre_init(void) { }
uint32 user_rf_cal_sector_set(void) { return 0x3FB; }
