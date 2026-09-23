/*
 * mycp.c - аналог утиліти cp, побудований ВИКЛЮЧНО на системних викликах Linux.
 *
 * Використані системні виклики:
 *   open(), read(), write(), close(), lseek()  - копіювання вмісту;
 *   fstat()                                    - метадані джерела за дескриптором;
 *   ftruncate()                                - точний розмір при копіюванні «дірок»;
 *   fchmod(), futimens()                       - збереження прав і часів (опція -p).
 *
 * Функції stdio (fopen/fread/fwrite/fseek) НЕ використовуються: уся робота йде
 * через файлові дескриптори, а помилки обробляються через errno/strerror().
 *
 * Використання: mycp [-nvpS] [-b РОЗМІР] ДЖЕРЕЛО ... ПРИЗНАЧЕННЯ
 */
#define _GNU_SOURCE
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DEFAULT_BUFSIZE (64u * 1024u)

struct opts {
    int no_clobber;       /* -n: не перезаписувати наявний файл призначення */
    int preserve;         /* -p: зберегти права доступу та часи */
    int verbose;          /* -v: друкувати, що саме скопійовано */
    int use_sparse;       /* -S: не записувати нульові «дірки» */
    size_t bufsize;       /* -b: розмір буфера читання/запису */
};

static void usage(void)
{
    fprintf(stderr,
            "Використання: mycp [-nvpS] [-b РОЗМІР] ДЖЕРЕЛО ... ПРИЗНАЧЕННЯ\n"
            "  -n         не перезаписувати наявний файл призначення\n"
            "  -p         зберегти права доступу та часи останньої зміни\n"
            "  -S         зберігати «дірки» розріджених файлів (SEEK_DATA/SEEK_HOLE)\n"
            "  -v         показувати хід копіювання\n"
            "  -b РОЗМІР  розмір буфера в байтах, суфікси K/M (типово 65536)\n");
}

/* Розбір аргументу -b: "65536", "64K", "1M". Повертає 0 при помилці. */
static size_t parse_size(const char *s)
{
    char *end = NULL;
    long long value = strtoll(s, &end, 10);
    long long mult = 1;

    if (end == s || value <= 0) {
        return 0;
    }
    if (*end == 'K' || *end == 'k') {
        mult = 1024;
        end++;
    } else if (*end == 'M' || *end == 'm') {
        mult = 1024 * 1024;
        end++;
    }
    if (*end != '\0') {
        return 0;
    }
    return (size_t)(value * mult);
}

/*
 * Копіює діапазон [start, end) з sfd у dfd.
 *
 * Саме тут видно, навіщо потрібен lseek(): перед початком діапазону обидва
 * дескриптори позиціонуються, а сам вміст переноситься парами read()/write()
 * через буфер заданого розміру. Повертає кількість скопійованих байтів
 * або -1 (errno встановлено).
 */
static off_t copy_range(int sfd, int dfd, char *buf, size_t bufsize, off_t start, off_t end)
{
    off_t remain = end - start;
    off_t copied = 0;

    if (lseek(sfd, start, SEEK_SET) < 0) {
        return -1;
    }
    if (lseek(dfd, start, SEEK_SET) < 0) {
        return -1;
    }

    while (remain > 0) {
        size_t want = (size_t)((remain < (off_t)bufsize) ? remain : (off_t)bufsize);
        ssize_t n = read_retry(sfd, buf, want);

        if (n < 0) {
            return -1;
        }
        if (n == 0) {
            break;                    /* файл джерела раптово закінчився */
        }
        if (write_all(dfd, buf, (size_t)n) < 0) {
            return -1;
        }
        copied += n;
        remain -= n;
    }
    return copied;
}

/*
 * Копіювання з урахуванням «дірок» (розріджені файли).
 *
 * Ідея: замість читати нулі з дірки й писати їх у призначення, ми питаємо у
 * файлової системи, де закінчується черговий блок даних (SEEK_DATA) і де
 * починається наступна дірка (SEEK_HOLE). Копіюємо лише блоки даних, а
 * позицію в призначенні пересуваємо через lseek() - «дірка» лишається діркою.
 * У кінці файл призначення розтягується до потрібного розміру через ftruncate().
 *
 * Якщо файлова система не підтримує SEEK_DATA/SEEK_HOLE, повертаємо -1 з
 * errno == EINVAL/ENOTSUP, і виклик переходить до звичайного копіювання.
 */
static off_t copy_sparse(int sfd, int dfd, char *buf, size_t bufsize, off_t size)
{
    off_t off = 0;
    off_t total = 0;

    while (off < size) {
        off_t data = lseek(sfd, off, SEEK_DATA);

        if (data < 0) {
            if (errno == ENXIO) {
                break;                /* далі до кінця файлу - суцільна дірка */
            }
            return -1;
        }
        off_t hole = lseek(sfd, data, SEEK_HOLE);
        if (hole < 0) {
            return -1;
        }
        off_t n = copy_range(sfd, dfd, buf, bufsize, data, hole);
        if (n < 0) {
            return -1;
        }
        total += n;
        off = hole;
    }

    if (ftruncate(dfd, size) < 0) {
        return -1;
    }
    return total;
}

/* Копіює один файл. Повертає 0 у разі успіху, -1 - при будь-якій помилці. */
static int copy_one(const char *src, const char *dst, const struct opts *o)
{
    int sfd;
    int dfd;
    char *buf;
    struct stat sst;
    mode_t mode;
    int flags;
    off_t copied;
    int failed = 0;

    sfd = open(src, O_RDONLY);
    if (sfd < 0) {
        warn_at("не вдалося відкрити джерело", src);
        return -1;
    }

    /* Метадані джерела беремо через fstat() - за вже відкритим дескриптором.
       Це надійніше за stat() за іменем: файл не можна підмінити між викликами. */
    if (fstat(sfd, &sst) < 0) {
        warn_at("fstat", src);
        close(sfd);
        return -1;
    }
    if (!S_ISREG(sst.st_mode)) {
        errno = EINVAL;
        warn_at("джерело не є звичайним файлом", src);
        close(sfd);
        return -1;
    }

    mode = o->preserve ? (sst.st_mode & 07777) : (mode_t)0644;
    flags = O_WRONLY | O_CREAT | (o->no_clobber ? O_EXCL : O_TRUNC);

    dfd = open(dst, flags, mode);
    if (dfd < 0) {
        if (errno == EEXIST) {
            warn_at("файл існує, не перезаписую (-n)", dst);
        } else {
            warn_at("не вдалося створити призначення", dst);
        }
        close(sfd);
        return -1;
    }

    buf = malloc(o->bufsize);
    if (buf == NULL) {
        errno = ENOMEM;
        warn_at("не вдалося виділити буфер", NULL);
        close(dfd);
        close(sfd);
        return -1;
    }

    copied = -1;
    if (o->use_sparse) {
        copied = copy_sparse(sfd, dfd, buf, o->bufsize, sst.st_size);
        if (copied < 0 && (errno == EINVAL || errno == ENOTSUP || errno == EOPNOTSUPP)) {
            err_msg("файлова система не підтримує SEEK_DATA/SEEK_HOLE - "
                    "копіюю звичайним способом");
            errno = 0;
            copied = copy_range(sfd, dfd, buf, o->bufsize, 0, sst.st_size);
        }
    } else {
        copied = copy_range(sfd, dfd, buf, o->bufsize, 0, sst.st_size);
    }

    if (copied < 0) {
        warn_at("помилка під час копіювання", src);
        failed = 1;
    }

    if (o->preserve) {
        /* Права і часи - теж системні виклики, а не бібліотечні обгортки.
           Для прав використовуємо fchmod() за дескриптором призначення. */
        if (fchmod(dfd, sst.st_mode & 07777) < 0) {
            warn_at("fchmod", dst);
            failed = 1;
        }
        struct timespec ts[2];
        ts[0] = sst.st_atim;
        ts[1] = sst.st_mtim;
        if (futimens(dfd, ts) < 0) {
            warn_at("futimens", dst);
            failed = 1;
        }
    }

    /* close() також повертає код помилки: дані могли не дійти до диска. */
    if (close(dfd) < 0) {
        warn_at("close", dst);
        failed = 1;
    }
    if (close(sfd) < 0) {
        warn_at("close", src);
        failed = 1;
    }
    free(buf);

    if (!failed && o->verbose) {
        char sz[32];

        human_size(sst.st_size, sz, sizeof sz);
        if (copied != sst.st_size) {
            /* Так буває при -S: у джерелі були «дірки», і ми їх не читали. */
            char read_sz[32];

            human_size(copied, read_sz, sizeof read_sz);
            printf("'%s' -> '%s' (%s; прочитано %s)\n", src, dst, sz, read_sz);
        } else {
            printf("'%s' -> '%s' (%s)\n", src, dst, sz);
        }
    }
    return failed ? -1 : 0;
}

int main(int argc, char *argv[])
{
    struct opts o = { 0, 0, 0, 0, DEFAULT_BUFSIZE };
    int opt;
    int nsrc;
    int status = EXIT_SUCCESS;
    int dst_is_dir = 0;
    const char *dst_arg;
    struct stat dst_st;
    int i;

    prog_init(argv[0]);

    while ((opt = getopt(argc, argv, "nvpSb:h")) != -1) {
        switch (opt) {
        case 'n':
            o.no_clobber = 1;
            break;
        case 'p':
            o.preserve = 1;
            break;
        case 'S':
            o.use_sparse = 1;
            break;
        case 'v':
            o.verbose = 1;
            break;
        case 'b':
            o.bufsize = parse_size(optarg);
            if (o.bufsize == 0) {
                err_msg("неправильний розмір буфера: '%s'", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'h':
            usage();
            return EXIT_SUCCESS;
        default:
            usage();
            return EXIT_FAILURE;
        }
    }

    nsrc = argc - optind;
    if (nsrc < 2) {
        err_msg("потрібно вказати джерело та призначення");
        usage();
        return EXIT_FAILURE;
    }

    dst_arg = argv[argc - 1];
    if (stat(dst_arg, &dst_st) == 0 && S_ISDIR(dst_st.st_mode)) {
        dst_is_dir = 1;                /* копіюємо у каталог, як справжній cp */
    } else if (nsrc > 2) {
        err_msg("призначення '%s' не є каталогом, а джерел декілька", dst_arg);
        return EXIT_FAILURE;
    }

    for (i = optind; i < argc - 1; i++) {
        char dst[4096];
        const char *base;
        int written;

        if (dst_is_dir) {
            const char *sep;
            size_t dlen;

            base = strrchr(argv[i], '/');
            base = (base != NULL) ? base + 1 : argv[i];
            if (base[0] == '\0') {
                err_msg("ім'я файлу джерела закінчується на '/': '%s'", argv[i]);
                status = EXIT_FAILURE;
                continue;
            }
            /* Якщо каталог призначення вже записано зі слешем на кінці,
               другий слеш не додаємо ("copy/" а не "copy//"). */
            dlen = strlen(dst_arg);
            sep = (dlen > 0 && dst_arg[dlen - 1] == '/') ? "" : "/";
            written = snprintf(dst, sizeof dst, "%s%s%s", dst_arg, sep, base);
        } else {
            written = snprintf(dst, sizeof dst, "%s", dst_arg);
        }
        if (written < 0 || (size_t)written >= sizeof dst) {
            errno = ENAMETOOLONG;
            warn_at("занадто довгий шлях", argv[i]);
            status = EXIT_FAILURE;
            continue;
        }
        if (copy_one(argv[i], dst, &o) < 0) {
            status = EXIT_FAILURE;
        }
    }
    return status;
}
