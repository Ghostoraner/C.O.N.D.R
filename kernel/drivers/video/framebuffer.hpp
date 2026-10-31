#pragma once
#include <nord/types.hpp>
#include <nord/limine.hpp>

namespace nord::video {

struct Color { u8 r, g, b; };

bool init(limine::Framebuffer* fb);
void clear(Color c);
void put_pixel(u64 x, u64 y, Color c);
void fill_rect(u64 x, u64 y, u64 w, u64 h, Color c);
void console_init();
void console_clear();
void console_putc(char c);
void console_set_color(Color color);
void console_reset_color();

u64 width();
u64 height();

}