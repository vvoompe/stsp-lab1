/*
 * mytree.c - рекурсивний обхід дерева каталогів системними викликами.
 *
 * Використані системні виклики:
 *   opendir(), readdir(), closedir()  - читання вмісту каталогу;
 *   stat(), lstat()                   - метадані кожного об'єкта за іменем;
 *   fstat()                           - метадані каталогу через дескриптор,
 *                                       який opendir() тримає всередині (dirfd);
 *   readlink()                        - ціль символьного посилання;
 *   write() (через write_all)         - виведення результату.
 *
 * Для кожного об'єкта збираються: тип, права доступу, розмір і час останньої
 * модифікації. Помилки доступу до окремих каталогів не зупиняють обхід:
 * вони друкуються через errno/strerror() і робота продовжується.
 *
 * Використання: mytree [-almb] [-d ГЛИБИНА] [ШЛЯХ ...]
 */
#define _GNU_SOURCE
#include "common.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_FOLLOW_DEPTH 40          /* запобіжник від циклів символьних посилань */

struct opts {
    int show_all;        /* -a: показувати приховані об'єкти (з крапки) */
    int max_depth;       /* -d N: обмеження глибини (0 - без обмеження) */
    int follow;          /* -l: заглядати всередину символьних посилань */
    int listing;         /* -m: плоский список метаданих замість дерева */
    int bytes;           /* -b: розміри у байтах (без людського формату) */
};

struct stats {
    long dirs;
    long files;
    long links;
    long other;
    long errors;
    off_t bytes;
};

static void usage(void)
{
    fprintf(stderr,
            "Використання: mytree [-almb] [-d ГЛИБИНА] [ШЛЯХ ...]\n"
            "  -a         показувати приховані об'єкти\n"
            "  -l         переходити всередину символьних посилань на каталоги\n"
            "  -m         плоский список метаданих (права, розмір, час, шлях)\n"
            "  -b         розміри у байтах, без людського формату\n"
            "  -d ГЛИБИНА обмежити глибину обходу (0 - без обмеження)\n");
}

/* Заголовок колонок метаданих. */
static void print_header(void)
{
    printf("%-10s %12s  %-19s  %s\n", "ПРАВА", "РОЗМІР", "МОДИФІКАЦІЯ", "ІМ'Я");
}

/* Один рядок метаданих: тип і права, розмір, час модифікації, ім'я. */
static void print_entry(const struct stat *st, const char *name, const char *target,
                        const char *prefix, const char *branch,
                        const struct opts *o, const char *suffix)
{
    char perm[11];
    char ts[32];
    char size[32];

    mode_string(st->st_mode, perm);
    time_string(st->st_mtime, ts, sizeof ts);
    if (o->bytes) {
        snprintf(size, sizeof size, "%lld", (long long)st->st_size);
    } else {
        human_size(st->st_size, size, sizeof size);
    }

    printf("%s%s%-10s %12s  %-19s  %s%s", prefix, branch, perm, size, ts, name, suffix);
    if (target != NULL) {
        printf(" -> %s", target);
    }
    putchar('\n');
}

/* Довільне завершення рядка шляху: формує "<dir>/<name>". */
static char *path_join(const char *dir, const char *name)
{
    size_t need = strlen(dir) + strlen(name) + 2;
    char *out = malloc(need);

    if (out == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    snprintf(out, need, "%s%s%s", dir,
             (dir[0] != '\0' && dir[strlen(dir) - 1] == '/') ? "" : "/", name);
    return out;
}

/* Будує рядок-префікс для дочірніх елементів дерева. */
static char *child_prefix(const char *prefix, int last)
{
    size_t need = strlen(prefix) + 8;
    char *out = malloc(need);

    if (out == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    snprintf(out, need, "%s%s", prefix, last ? "    " : "│   ");
    return out;
}

static int cmp_name(const void *a, const void *b)
{
    const char *const *sa = (const char *const *)a;
    const char *const *sb = (const char *const *)b;

    return strcmp(*sa, *sb);
}

/* Копіює ціль символьного посилання через readlink(). Повертає malloc-рядок. */
static char *readlink_dup(const char *path)
{
    size_t cap = 256;

    for (;;) {
        char *buf = malloc(cap);

        if (buf == NULL) {
            errno = ENOMEM;
            return NULL;
        }
        ssize_t n = readlink(path, buf, cap - 1);
        if (n < 0) {
            free(buf);
            return NULL;
        }
        if ((size_t)n < cap - 1) {
            buf[n] = '\0';
            return buf;
        }
        free(buf);
        cap *= 2;                        /* ціль довша за буфер - пробуємо більше */
    }
}

/*
 * Читає імена з відкритого каталогу, відкидає "." та ".." і сортує.
 * Повертає масив рядків (треба звільняти через free_names) або NULL.
 */
static char **read_names(DIR *dp, const struct opts *o, long *count)
{
    char **names = NULL;
    long n = 0;
    long cap = 0;
    struct dirent *de;

    errno = 0;
    while ((de = readdir(dp)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;                    /* службові посилання на себе і батька */
        }
        if (!o->show_all && de->d_name[0] == '.') {
            continue;
        }
        if (n == cap) {
            long ncap = (cap > 0) ? cap * 2 : 16;
            char **tmp = realloc(names, (size_t)ncap * sizeof *names);

            if (tmp == NULL) {
                errno = ENOMEM;
                goto fail;
            }
            names = tmp;
            cap = ncap;
        }
        names[n] = strdup(de->d_name);
        if (names[n] == NULL) {
            errno = ENOMEM;
            goto fail;
        }
        n++;
    }
    /* readdir() повертає NULL і в кінці каталогу, і при помилці - розрізняє errno. */
    if (errno != 0) {
        goto fail;
    }

    if (n > 1) {
        qsort(names, (size_t)n, sizeof *names, cmp_name);
    }
    *count = n;
    return names;

fail:
    for (long i = 0; i < n; i++) {
        free(names[i]);
    }
    free(names);
    *count = 0;
    return NULL;
}

static void free_names(char **names, long n)
{
    for (long i = 0; i < n; i++) {
        free(names[i]);
    }
    free(names);
}

/* Рекурсивний обхід каталогу. path - шлях каталогу, prefix - графіка дерева. */
static void walk_dir(const char *path, const char *prefix, int depth,
                     const struct opts *o, struct stats *s)
{
    DIR *dp = opendir(path);
    if (dp == NULL) {
        warn_at("не вдалося відкрити каталог", path);
        s->errors++;
        return;
    }

    /* Метадані самого каталогу беремо через fstat() за дескриптором dirfd(),
       який opendir() вже тримає відкритим: окремий виклик stat() не потрібен
       і неможлива ситуація, коли каталог підмінили між двома викликами. */
    struct stat self;
    if (fstat(dirfd(dp), &self) < 0) {
        warn_at("fstat", path);
        s->errors++;
    }

    long n = 0;
    char **names = read_names(dp, o, &n);
    if (names == NULL && n == 0) {
        if (errno != 0) {
            warn_at("не вдалося прочитати каталог", path);
            s->errors++;
        }
        closedir(dp);
        return;
    }

    for (long i = 0; i < n; i++) {
        int last = (i == n - 1);
        const char *branch = last ? "└── " : "├── ";
        char *child = path_join(path, names[i]);
        struct stat lst;

        if (child == NULL) {
            warn_at("не вдалося побудувати шлях", names[i]);
            s->errors++;
            continue;
        }

        /* lstat() дає інформацію про сам об'єкт, не розкриваючи посилання. */
        if (lstat(child, &lst) < 0) {
            warn_at("lstat", child);
            s->errors++;
            free(child);
            continue;
        }

        char *target = NULL;
        if (S_ISLNK(lst.st_mode)) {
            target = readlink_dup(child);
            s->links++;
        } else if (S_ISDIR(lst.st_mode)) {
            s->dirs++;
        } else if (S_ISREG(lst.st_mode)) {
            s->files++;
            s->bytes += lst.st_size;
        } else {
            s->other++;                  /* пристрої, сокети, канали */
        }

        int is_dir = S_ISDIR(lst.st_mode);
        if (!is_dir && o->follow && S_ISLNK(lst.st_mode)) {
            struct stat eff;
            if (stat(child, &eff) == 0 && S_ISDIR(eff.st_mode)) {
                is_dir = 1;
            }
        }

        if (o->listing) {
            print_entry(&lst, child, target, "", "", o, "");
        } else {
            print_entry(&lst, names[i], target, prefix, branch, o, is_dir ? "/" : "");
        }
        free(target);

        if (is_dir && (o->max_depth == 0 || depth < o->max_depth) &&
            depth < MAX_FOLLOW_DEPTH) {
            char *cprefix = child_prefix(prefix, last);

            if (cprefix == NULL) {
                warn_at("не вдалося виділити пам'ять", child);
                s->errors++;
            } else {
                walk_dir(child, cprefix, depth + 1, o, s);
                free(cprefix);
            }
        }
        free(child);
    }

    free_names(names, n);
    if (closedir(dp) < 0) {
        warn_at("closedir", path);
        s->errors++;
    }
}

/* Обробляє один аргумент командного рядка (каталог або звичайний файл). */
static void walk_root(const char *path, const struct opts *o, struct stats *s)
{
    struct stat st;

    if (lstat(path, &st) < 0) {
        warn_at("lstat", path);
        s->errors++;
        return;
    }

    if (o->listing) {
        print_entry(&st, path, NULL, "", "", o, "");
    } else {
        printf("%s\n", path);
    }

    if (!S_ISDIR(st.st_mode)) {
        if (S_ISREG(st.st_mode)) {
            s->files++;
            s->bytes += st.st_size;
        } else if (S_ISLNK(st.st_mode)) {
            s->links++;
        } else {
            s->other++;
        }
        return;
    }

    s->dirs++;
    walk_dir(path, "", 1, o, s);
}

int main(int argc, char *argv[])
{
    struct opts o = { 0, 0, 0, 0, 0 };
    struct stats s = { 0, 0, 0, 0, 0, 0 };
    int opt;
    int i;

    prog_init(argv[0]);

    while ((opt = getopt(argc, argv, "almbd:h")) != -1) {
        switch (opt) {
        case 'a':
            o.show_all = 1;
            break;
        case 'l':
            o.follow = 1;
            break;
        case 'm':
            o.listing = 1;
            break;
        case 'b':
            o.bytes = 1;
            break;
        case 'd':
            o.max_depth = atoi(optarg);
            if (o.max_depth < 0) {
                err_msg("глибина не може бути від'ємною: '%s'", optarg);
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

    if (o.listing) {
        print_header();
    }

    if (optind >= argc) {
        walk_root(".", &o, &s);           /* без аргументів - поточний каталог */
    } else {
        for (i = optind; i < argc; i++) {
            walk_root(argv[i], &o, &s);
        }
    }

    char total[32];

    fflush(stdout);        /* підсумок іде у stderr - спершу дописуємо stdout,
                              інакше при перенаправленні рядки переплутаються */
    human_size(s.bytes, total, sizeof total);
    fprintf(stderr,
            "\nРазом: каталогів %ld, звичайних файлів %ld, посилань %ld, інших %ld; "
            "сумарний розмір файлів %s\n",
            s.dirs, s.files, s.links, s.other, total);
    if (s.errors > 0) {
        fprintf(stderr, "Помилок доступу: %ld\n", s.errors);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
