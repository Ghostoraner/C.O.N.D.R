#include <drivers/video/framebuffer.hpp>
#include <lib/print.hpp>

namespace nord::video {

extern "C" const u8 _binary_kernel_drivers_video_font12x22_bin_start[];

static u32* g_addr   = nullptr;
static u64  g_width  = 0;
static u64  g_height = 0;
static u64  g_pitch32 = 0;
static limine::Framebuffer* g_fb = nullptr;
static constexpr u64 FONT_WIDTH = 12;
static constexpr u64 FONT_HEIGHT = 22;
static constexpr u64 FONT_ROW_BYTES = 2;
static constexpr u64 FONT_GLYPH_BYTES = FONT_HEIGHT * FONT_ROW_BYTES;
static constexpr u64 CELL_WIDTH = FONT_WIDTH;
static constexpr u64 CELL_HEIGHT = FONT_HEIGHT;
static constexpr Color BACKGROUND = {12, 12, 12};
static constexpr Color FOREGROUND = {204, 204, 204};
static constexpr Color ACCENT = {76, 175, 80};
static Color g_foreground = FOREGROUND;
static u64 g_cursor_x = 16;
static u64 g_cursor_y = 52;

u64 width()  { return g_width; }
u64 height() { return g_height; }

bool init(limine::Framebuffer* fb) {
    if (!fb || fb->bpp != 32) return false;
    g_fb      = fb;
    g_addr    = static_cast<u32*>(fb->address);
    g_width   = fb->width;
    g_height  = fb->height;
    g_pitch32 = fb->pitch / 4;
    kprintf("[fb] %lux%lu, %u bpp, pitch %lu\n", g_width, g_height,
            static_cast<u32>(fb->bpp), fb->pitch);
    return true;
}

static inline u32 encode(Color c) {
    return (static_cast<u32>(c.r) << g_fb->red_mask_shift)
         | (static_cast<u32>(c.g) << g_fb->green_mask_shift)
         | (static_cast<u32>(c.b) << g_fb->blue_mask_shift);
}

void put_pixel(u64 x, u64 y, Color c) {
    if (!g_addr || x >= g_width || y >= g_height) return;
    g_addr[y * g_pitch32 + x] = encode(c);
}

void fill_rect(u64 x, u64 y, u64 w, u64 h, Color c) {
    if (!g_addr) return;
    u32 v = encode(c);
    for (u64 j = y; j < y + h && j < g_height; ++j) {
        u32* row = g_addr + j * g_pitch32;
        for (u64 i = x; i < x + w && i < g_width; ++i) row[i] = v;
    }
}

void clear(Color c) { fill_rect(0, 0, g_width, g_height, c); }

static void draw_character(char character) {
    const u8* glyph = _binary_kernel_drivers_video_font12x22_bin_start
        + static_cast<u8>(character) * FONT_GLYPH_BYTES;
    for (u64 y = 0; y < FONT_HEIGHT; ++y) {
        const u16 row_bits = (static_cast<u16>(glyph[y * FONT_ROW_BYTES]) << 8)
            | glyph[y * FONT_ROW_BYTES + 1];
        for (u64 x = 0; x < FONT_WIDTH; ++x) {
            const Color color = (row_bits & (0x8000U >> x)) ? g_foreground : BACKGROUND;
            put_pixel(g_cursor_x + x, g_cursor_y + y, color);
        }
    }
    g_cursor_x += CELL_WIDTH;
}

void console_clear() {
    clear(BACKGROUND);
    g_cursor_x = 16;
    g_cursor_y = 52;
    fill_rect(16, 36, g_width > 32 ? g_width - 32 : 0, 2, ACCENT);
}

void console_init() {
    g_foreground = FOREGROUND;
    console_clear();
}

void console_set_color(Color color) { g_foreground = color; }
void console_reset_color() { g_foreground = FOREGROUND; }

static void scroll_console() {
    if (g_height <= CELL_HEIGHT) return;
    const u64 rows = (g_height - CELL_HEIGHT) * g_pitch32;
    for (u64 i = 0; i < rows; ++i) g_addr[i] = g_addr[i + CELL_HEIGHT * g_pitch32];
    fill_rect(0, g_height - CELL_HEIGHT, g_width, CELL_HEIGHT, BACKGROUND);
    g_cursor_y = g_height - CELL_HEIGHT;
}

void console_putc(char character) {
    if (!g_addr) return;
    const u8 byte = static_cast<u8>(character);
    if (byte >= 0x80) return;
    if (character == '\r') {
        g_cursor_x = 16;
        return;
    }
    if (character == '\n') {
        g_cursor_x = 16;
        g_cursor_y += CELL_HEIGHT;
    } else if (character == '\b') {
        if (g_cursor_x > 16) {
            g_cursor_x -= CELL_WIDTH;
            fill_rect(g_cursor_x, g_cursor_y, CELL_WIDTH, CELL_HEIGHT, BACKGROUND);
        }
        return;
    } else if (character == '\t') {
        g_cursor_x = ((g_cursor_x / (CELL_WIDTH * 4)) + 1) * (CELL_WIDTH * 4);
    } else if (static_cast<u8>(character) >= 0x20) {
        draw_character(character);
    } else {
        return;
    }

    if (g_cursor_x + CELL_WIDTH > g_width) {
        g_cursor_x = 16;
        g_cursor_y += CELL_HEIGHT;
    }
    if (g_cursor_y + CELL_HEIGHT > g_height) scroll_console();
}

}