#pragma once
#include <nord/types.hpp>

extern "C" {
void* memcpy(void* dst, const void* src, nord::usize n);
void* memset(void* dst, int c, nord::usize n);
void* memmove(void* dst, const void* src, nord::usize n);
int   memcmp(const void* a, const void* b, nord::usize n);
nord::usize strlen(const char* s);
int   strcmp(const char* a, const char* b);
int   strncmp(const char* a, const char* b, nord::usize n);
}