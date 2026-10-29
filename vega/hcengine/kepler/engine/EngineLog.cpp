/**
 * The engine's own log file, without taking over the app's stdout/stderr.
 *
 * HamClock's makeDiagFile() rolls diagnostic-log*.txt and dup2()s the new file onto fds 1 and 2,
 * which on a desktop is the whole process. Embedded, that captured the app's native output too
 * (the Vega runtime writes lines like "Key not found" there by the hundred), so the diagnostic
 * log that HamClock's postDiags uploads, and getLogTail, were mostly noise.
 *
 * Linker wraps (-Wl,--wrap=..., libHcEngine.so only) route by call site instead:
 *   - dup2(fd, 1 or 2) keeps a copy of fd as the engine log and leaves fds 1 and 2 alone;
 *   - stdio calls in this library aimed at stdout/stderr (HamClock's printf/Serial/fprintf, and
 *     the host's own "HcEngine: ..." lines) go to that file. Other FILEs pass straight through.
 * The rest of the process never goes through these wraps, so its output stays out of the file.
 * Before HamClock's first makeDiagFile, output goes to the real stdout/stderr as before.
 */
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unistd.h>

extern "C" {
int __real_dup2(int oldfd, int newfd);
int __real_printf(const char *fmt, ...);
int __real_vprintf(const char *fmt, va_list ap);
int __real_fprintf(FILE *f, const char *fmt, ...);
int __real_vfprintf(FILE *f, const char *fmt, va_list ap);
int __real_fputs(const char *s, FILE *f);
int __real_puts(const char *s);
int __real_putchar(int c);
size_t __real_fwrite(const void *p, size_t size, size_t n, FILE *f);
int __real_fflush(FILE *f);
ssize_t __real_write(int fd, const void *buf, size_t n);
}

namespace {

std::mutex lmu;
FILE *g_log = nullptr;  // engine log (current diagnostic-log.txt), or null before the first run

bool isStd(FILE *f) { return f == stdout || f == stderr; }

}  // namespace

extern "C" {

int __wrap_dup2(int oldfd, int newfd) {
  if (newfd != 1 && newfd != 2) return __real_dup2(oldfd, newfd);
  if (newfd == 1) {  // makeDiagFile does 1 then 2 with the same fd: open the file once
    int d = dup(oldfd);
    if (d < 0) return -1;
    FILE *f = fdopen(d, "a");
    if (!f) { int e = errno; close(d); errno = e; return -1; }
    setvbuf(f, nullptr, _IOLBF, 0);
    std::lock_guard<std::mutex> l(lmu);
    if (g_log) fclose(g_log);
    g_log = f;
  }
  return newfd;
}

int __wrap_vfprintf(FILE *f, const char *fmt, va_list ap) {
  if (isStd(f)) {
    std::lock_guard<std::mutex> l(lmu);
    if (g_log) return __real_vfprintf(g_log, fmt, ap);
  }
  return __real_vfprintf(f, fmt, ap);
}

int __wrap_fprintf(FILE *f, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = __wrap_vfprintf(f, fmt, ap);
  va_end(ap);
  return n;
}

int __wrap_vprintf(const char *fmt, va_list ap) { return __wrap_vfprintf(stdout, fmt, ap); }

int __wrap_printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = __wrap_vfprintf(stdout, fmt, ap);
  va_end(ap);
  return n;
}

int __wrap_fputs(const char *s, FILE *f) {
  if (isStd(f)) {
    std::lock_guard<std::mutex> l(lmu);
    if (g_log) return __real_fputs(s, g_log);
  }
  return __real_fputs(s, f);
}

// The compiler may turn printf("text\n") into puts and printf("c") into putchar.
int __wrap_puts(const char *s) {
  {
    std::lock_guard<std::mutex> l(lmu);
    if (g_log) return __real_fputs(s, g_log) < 0 ? EOF : (fputc('\n', g_log) == EOF ? EOF : 1);
  }
  return __real_puts(s);
}

int __wrap_putchar(int c) {
  {
    std::lock_guard<std::mutex> l(lmu);
    if (g_log) return fputc(c, g_log);
  }
  return __real_putchar(c);
}

size_t __wrap_fwrite(const void *p, size_t size, size_t n, FILE *f) {
  if (isStd(f)) {
    std::lock_guard<std::mutex> l(lmu);
    if (g_log) return __real_fwrite(p, size, n, g_log);
  }
  return __real_fwrite(p, size, n, f);
}

int __wrap_fflush(FILE *f) {
  if (!f || isStd(f)) {
    std::lock_guard<std::mutex> l(lmu);
    if (g_log) __real_fflush(g_log);
    if (f) return 0;
  }
  return __real_fflush(f);
}

ssize_t __wrap_write(int fd, const void *buf, size_t n) {
  if (fd == 1 || fd == 2) {
    std::lock_guard<std::mutex> l(lmu);
    if (g_log) {
      __real_fflush(g_log);
      return __real_write(fileno(g_log), buf, n);
    }
  }
  return __real_write(fd, buf, n);
}

}  // extern "C"
