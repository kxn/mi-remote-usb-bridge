/* Newlib syscall stubs not provided by the WCH SDK's CH58x_sys.c
 * (which supplies _write/_sbrk/_read?).  Bridge firmware has no
 * filesystem: reads fail, the rest are no-ops. */
#include <sys/stat.h>
#include <errno.h>

#undef errno
extern int errno;

int _read(int fd, char *buf, int len)
{
    (void)fd;
    (void)buf;
    (void)len;
    errno = ENOSYS;
    return -1;
}

int _close(int fd)
{
    (void)fd;
    errno = ENOSYS;
    return -1;
}

int _fstat(int fd, struct stat *st)
{
    (void)fd;
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int fd)
{
    (void)fd;
    return 1;
}

int _lseek(int fd, int ptr, int dir)
{
    (void)fd;
    (void)ptr;
    (void)dir;
    errno = ENOSYS;
    return -1;
}
