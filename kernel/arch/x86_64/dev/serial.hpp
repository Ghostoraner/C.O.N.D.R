#pragma once
#include <nord/types.hpp>

namespace nord::arch::serial {

bool init();
void putc(char c);
void write(const char* s);
char getc();
bool try_getc(char& c);

}