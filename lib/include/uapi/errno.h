#ifndef EUNUCHS_UAPI_ERRNO_H
#define EUNUCHS_UAPI_ERRNO_H

/* Error numbers (Linux values). Syscalls return -errno. */
#define ERRNOS(X)                                   \
    X(EPERM,    1,  "Operation not permitted")      \
    X(ENOENT,   2,  "No such file or directory")    \
    X(ESRCH,    3,  "No such process")              \
    X(EINTR,    4,  "Interrupted system call")      \
    X(EIO,      5,  "I/O error")                    \
    X(ENXIO,    6,  "No such device or address")    \
    X(E2BIG,    7,  "Argument list too long")       \
    X(ENOEXEC,  8,  "Exec format error")            \
    X(EBADF,    9,  "Bad file descriptor")          \
    X(ECHILD,   10, "No child processes")           \
    X(EAGAIN,   11, "Try again")                    \
    X(ENOMEM,   12, "Out of memory")                \
    X(EACCES,   13, "Permission denied")            \
    X(EFAULT,   14, "Bad address")                  \
    X(EBUSY,    16, "Device or resource busy")      \
    X(EEXIST,   17, "File exists")                  \
    X(EXDEV,    18, "Cross-device link")            \
    X(ENODEV,   19, "No such device")               \
    X(ENOTDIR,  20, "Not a directory")              \
    X(EISDIR,   21, "Is a directory")               \
    X(EINVAL,   22, "Invalid argument")             \
    X(ENFILE,   23, "File table overflow")          \
    X(EMFILE,   24, "Too many open files")          \
    X(ENOTTY,   25, "Inappropriate ioctl for device") \
    X(EFBIG,    27, "File too large")               \
    X(ENOSPC,   28, "No space left on device")      \
    X(ESPIPE,   29, "Illegal seek")                 \
    X(EROFS,    30, "Read-only file system")        \
    X(EMLINK,   31, "Too many links")               \
    X(EPIPE,    32, "Broken pipe")                  \
    X(ERANGE,   34, "Result too large")             \
    X(ENAMETOOLONG, 36, "File name too long")       \
    X(ENOSYS,   38, "Function not implemented")     \
    X(ENOTEMPTY, 39, "Directory not empty")         \
    X(ENOTSOCK, 88, "Socket operation on non-socket") \
    X(EDESTADDRREQ, 89, "Destination address required") \
    X(EMSGSIZE, 90, "Message too long")             \
    X(ENOPROTOOPT, 92, "Protocol not available")    \
    X(EPROTONOSUPPORT, 93, "Protocol not supported") \
    X(EOPNOTSUPP, 95, "Operation not supported")    \
    X(EAFNOSUPPORT, 97, "Address family not supported") \
    X(EADDRINUSE, 98, "Address already in use")     \
    X(EADDRNOTAVAIL, 99, "Cannot assign requested address") \
    X(ENETDOWN, 100, "Network is down")             \
    X(ENETUNREACH, 101, "Network is unreachable")   \
    X(ECONNABORTED, 103, "Connection aborted")      \
    X(ECONNRESET, 104, "Connection reset by peer")  \
    X(ENOBUFS,  105, "No buffer space available")   \
    X(EISCONN,  106, "Transport endpoint is already connected") \
    X(ENOTCONN, 107, "Transport endpoint is not connected") \
    X(ETIMEDOUT, 110, "Connection timed out")       \
    X(ECONNREFUSED, 111, "Connection refused")      \
    X(EHOSTUNREACH, 113, "No route to host")        \
    X(EALREADY, 114, "Operation already in progress") \
    X(EINPROGRESS, 115, "Operation now in progress") \
    X(ESTALE,   116, "Stale file handle")

enum {
#define X(name, num, desc) name = num,
    ERRNOS(X)
#undef X
};

#endif
