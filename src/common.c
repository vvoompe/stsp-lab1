/*
 * common.c - реалізація спільних допоміжних функцій (див. common.h).
 */
#include "common.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *g_prog = "myutil";

void prog_init(const char *argv0)
{
    /* У повідомленнях показуємо лише ім'я файлу, без повного шляху запуску. */
    const char *slash = strrchr(argv0, '/');
    g_prog = (slash != NULL) ? slash + 1 : argv0;
}

void warn_at(const char *what, const char *name)
{
    int saved = errno;               /* зберігаємо: fprintf() має право змінити errno */

    if (name != NULL) {
        fprintf(stderr, "%s: %s '%s': %s\n", g_prog, what, name, strerror(saved));
    } else {
        fprintf(stderr, "%s: %s: %s\n", g_prog, what, strerror(saved));
    }
}

void die_at(const char *what, const char *name)
{
    warn_at(what, name);
    exit(EXIT_FAILURE);
}

void err_msg(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s: ", g_prog);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

ssize_t write_all(int fd, const void *buf, size_t count)
{
    const char *p = (const char *)buf;
    size_t done = 0;

    while (done < count) {
        ssize_t n = write(fd, p + done, count - done);

        if (n < 0) {
            if (errno == EINTR) {
                continue;            /* сигнал перервав виклик - просто повторюємо */
            }
            return -1;
        }
        if (n == 0) {
            errno = EIO;             /* не має траплятись: запис нуля байтів без помилки */
            return -1;
        }
        done += (size_t)n;
    }
    return (ssize_t)done;
}

ssize_t read_retry(int fd, void *buf, size_t count)
{
    for (;;) {
        ssize_t n = read(fd, buf, count);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        return n;
    }
}

void human_size(off_t size, char *out, size_t outsz)
{
    static const char *units[] = { "Б", "КіБ", "МіБ", "ГіБ", "ТіБ" };
    double value = (double)size;
    int i = 0;

    while (value >= 1024.0 && i < 4) {
        value /= 1024.0;
        i++;
    }
    if (i == 0) {
        snprintf(out, outsz, "%lld Б", (long long)size);
    } else {
        snprintf(out, outsz, "%.1f %s", value, units[i]);
    }
}

void mode_string(mode_t mode, char out[11])
{
    static const char rwx[] = "rwxrwxrwx";
    int i;

    if (S_ISDIR(mode)) {
        out[0] = 'd';
    } else if (S_ISLNK(mode)) {
        out[0] = 'l';
    } else if (S_ISCHR(mode)) {
        out[0] = 'c';
    } else if (S_ISBLK(mode)) {
        out[0] = 'b';
    } else if (S_ISFIFO(mode)) {
        out[0] = 'p';
    } else if (S_ISSOCK(mode)) {
        out[0] = 's';
    } else {
        out[0] = '-';
    }

    for (i = 0; i < 9; i++) {
        out[1 + i] = (mode & (mode_t)(1u << (8 - i))) ? rwx[i] : '-';
    }

    /* Специфічні біти: setuid, setgid, sticky - показуємо як маленькі s/t. */
    if (mode & S_ISUID) {
        out[3] = (out[3] == 'x') ? 's' : 'S';
    }
    if (mode & S_ISGID) {
        out[6] = (out[6] == 'x') ? 's' : 'S';
    }
    if (mode & S_ISVTX) {
        out[9] = (out[9] == 'x') ? 't' : 'T';
    }
    out[10] = '\0';
}

void time_string(time_t t, char *out, size_t outsz)
{
    struct tm tm;

    if (localtime_r(&t, &tm) == NULL) {
        snprintf(out, outsz, "?");
        return;
    }
    strftime(out, outsz, "%Y-%m-%d %H:%M:%S", &tm);
}
