// string ops, freestanding replacements
#pragma once
#include <stddef.h>
#include <stdint.h>

void *memset(void *dst, int val, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
char *strcpy(char *dst, const char *src);
int strncmp(const char *a, const char *b, size_t n);
int memcmp(const void *a, const void *b, size_t n);
char *strchr(const char *s, int c);
char *strncpy(char *dst, const char *src, size_t n);
