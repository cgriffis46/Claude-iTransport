/*
 * syscalls.c — newlib's I/O hooks. Nothing here uses files; stdio code
 * pulled in by snprintf still refers to these. log.cpp does the output.
 */
#include <errno.h>
#include <sys/stat.h>

int _close(int fd) { (void)fd; errno = EBADF; return -1; }
int _lseek(int fd, int off, int dir) { (void)fd; (void)off; (void)dir; return 0; }
int _read(int fd, char *buf, int len) { (void)fd; (void)buf; (void)len; return 0; }
int _write(int fd, const char *buf, int len) { (void)fd; (void)buf; return len; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
