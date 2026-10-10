#include <debug.h>
#include <kstring.h>
#include <safe.h>

void hexdump_safe(print_fn print, uintptr_t addr, size_t len)
{
    for (size_t off = 0; off < len; off += 16) {
        char line[96];
        char *p = line;
        u8 bytes[16];
        bool ok[16];

        for (int j = 0; j < 16; j++)
            ok[j] = off + j < len && safe_read_u8(addr + off + j, &bytes[j]) == 0;
        for (int j = 0; j < 16; j++) {
            static const char hex[] = "0123456789abcdef";
            if (off + j >= len) {
                *p++ = ' '; *p++ = ' ';
            } else if (ok[j]) {
                *p++ = hex[bytes[j] >> 4];
                *p++ = hex[bytes[j] & 15];
            } else {
                *p++ = '?'; *p++ = '?';
            }
            *p++ = ' ';
            if (j == 7)
                *p++ = ' ';
        }
        *p++ = '|';
        for (int j = 0; j < 16 && off + j < len; j++)
            *p++ = ok[j] && kisprint(bytes[j]) ? (char)bytes[j] : '.';
        *p++ = '|';
        *p = '\0';
        print("%016lx: %s\n", (unsigned long)(addr + off), line);
    }
}
