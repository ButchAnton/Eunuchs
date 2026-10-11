#ifndef EUNUCHS_UAPI_TERMIOS_H
#define EUNUCHS_UAPI_TERMIOS_H

/* Terminal interface (Linux layout and values; a subset is implemented). */

#include <stdint.h>

typedef uint32_t tcflag_t;
typedef uint8_t cc_t;

#define NCCS 19
struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_line;
    cc_t c_cc[NCCS];
};

/* c_cc indexes */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSUSP    10
#define VWERASE  14

/* c_iflag */
#define IGNCR    0000200
#define ICRNL    0000400
#define IXON     0002000
/* c_oflag */
#define OPOST    0000001
#define ONLCR    0000004
/* c_cflag */
#define CS8      0000060
#define CREAD    0000200
/* c_lflag */
#define ISIG     0000001
#define ICANON   0000002
#define ECHO     0000010
#define ECHOE    0000020
#define ECHOK    0000040
#define ECHONL   0000100
#define NOFLSH   0000200
#define ECHOCTL  0001000
#define IEXTEN   0100000

/* ioctl requests */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCSCTTY  0x540E
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define FIONREAD   0x541B
#define TIOCNOTTY  0x5422

struct winsize {
    uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel;
};

#define O_NOCTTY 0400

#endif
