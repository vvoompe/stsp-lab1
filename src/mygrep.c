/*
 * mygrep.c - аналог утиліти grep, побудований на системних викликах Linux.
 *
 * Використані системні виклики:
 *   open(), read(), write(), close()  - введення з файлів і виведення результату;
 *   lseek()                           - позиціювання для опції -b (зміщення у байтах)
 *                                       та перевірки, що потік можна «відмотувати»;
 *   fstat()                           - визначення, чи є дескриптор звичайним файлом
 *                                       (канал/термінал через lseek дає ESPIPE).
 *
 * Функції stdio для читання файлів не використовуються. Рядки збираються
 * вручну з буфера read(), тому утиліта коректно працює й з рядками, довшими
 * за розмір буфера.
 *
 * Використання: mygrep [-ivcnqswb] ШАБЛОН [ФАЙЛ ...]
 */
#define _GNU_SOURCE
#include "common.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define READ_CHUNK (64u * 1024u)
#define LINE_INIT  1024u

struct opts {
    int icase;            /* -i: не враховувати регістр */
    int invert;           /* -v: виводити рядки, що НЕ збігаються */
    int count_only;       /* -c: лише кількість збігів */
    int number;           /* -n: друкувати номер рядка */
    int quiet;            /* -q: нічого не виводити, лише код завершення */
    int silent;           /* -s: не повідомляти про помилки читання */
    int word;             /* -w: збіг лише як окреме слово */
    int byte_off;         /* -b: друкувати зміщення збігу в байтах (lseek) */
};

static void usage(void)
{
    fprintf(stderr,
            "Використання: mygrep [-ivcnqswb] ШАБЛОН [ФАЙЛ ...]\n"
            "  -i  ігнорувати регістр літер (латиниця та кирилиця)\n"
            "  -v  виводити рядки, які НЕ містять шаблон\n"
            "  -c  виводити лише кількість рядків зі збігами\n"
            "  -n  додавати номер рядка\n"
            "  -q  жодного виводу, лише код завершення (0 - знайдено)\n"
            "  -s  придушувати повідомлення про помилки читання\n"
            "  -w  шукати шаблон як окреме слово\n"
            "  -b  додавати зміщення збігу в байтах (визначається через lseek)\n"
            "Файл '-' або відсутність файлів означає читання зі стандартного входу.\n");
}

/* Буферизоване читання дескриптора через read(). */
struct reader {
    int fd;
    int seekable;      /* 1 - звичайний файл, позицію можна спитати через lseek() */
    char *buf;
    size_t cap;
    size_t pos;        /* поточна позиція в буфері */
    size_t len;        /* скільки байтів у буфері прочитано */
    off_t total;       /* усього прочитано з дескриптора */
    int saw_nul;       /* у даних траплявся нульовий байт (схоже на двійковий файл) */
};

static int reader_init(struct reader *r, int fd)
{
    struct stat st;

    r->fd = fd;
    r->pos = 0;
    r->len = 0;
    r->total = 0;
    r->saw_nul = 0;
    r->cap = READ_CHUNK;
    r->buf = malloc(r->cap);
    if (r->buf == NULL) {
        errno = ENOMEM;
        return -1;
    }
    /* fstat() каже, з чим ми маємо справу: для звичайного файлу позицію можна
       питати через lseek(), для каналу/термінала - ні (там буде ESPIPE). */
    r->seekable = (fstat(fd, &st) == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
    return 0;
}

static void reader_free(struct reader *r)
{
    free(r->buf);
    r->buf = NULL;
}

/* Підтягує наступну порцію байтів у буфер. Повертає кількість доступних
   байтів, 0 - кінець файлу, -1 - помилка (errno встановлено). */
static ssize_t reader_fill(struct reader *r)
{
    ssize_t n;

    if (r->pos < r->len) {
        return (ssize_t)(r->len - r->pos);
    }
    n = read_retry(r->fd, r->buf, r->cap);
    if (n <= 0) {
        return n;
    }
    r->pos = 0;
    r->len = (size_t)n;
    r->total += n;
    if (!r->saw_nul && memchr(r->buf, '\0', (size_t)n) != NULL) {
        r->saw_nul = 1;
    }
    return n;
}

/* Поточна позиція у потоці: справжнє lseek() для звичайних файлів
   і арифметика за прочитаними байтами для каналів. */
static off_t reader_tell(struct reader *r)
{
    off_t unread = (off_t)(r->len - r->pos);

    if (r->seekable) {
        off_t cur = lseek(r->fd, 0, SEEK_CUR);

        if (cur >= 0) {
            return cur - unread;
        }
    }
    return r->total - unread;
}

/*
 * Читає один рядок. Повертає 1 - рядок отримано, 0 - кінець файлу, -1 - помилка.
 * Рядок повертається без '\n' у *line (буфер нарощується через realloc, тому
 * довжина рядка не обмежена розміром буфера read()).
 */
static int reader_line(struct reader *r, char **line, size_t *linelen,
                       size_t *linecap, off_t *offset)
{
    *linelen = 0;
    *offset = reader_tell(r);

    for (;;) {
        ssize_t avail = reader_fill(r);

        if (avail < 0) {
            return -1;
        }
        if (avail == 0) {
            return (*linelen > 0) ? 1 : 0;   /* останній рядок без '\n' */
        }

        char *nl = memchr(r->buf + r->pos, '\n', (size_t)avail);
        size_t take = (nl != NULL) ? (size_t)(nl - (r->buf + r->pos)) : (size_t)avail;

        if (*linelen + take + 1 > *linecap) {
            size_t ncap = (*linecap > 0) ? *linecap : LINE_INIT;

            while (ncap < *linelen + take + 1) {
                ncap *= 2;
            }
            char *tmp = realloc(*line, ncap);
            if (tmp == NULL) {
                errno = ENOMEM;
                return -1;
            }
            *line = tmp;
            *linecap = ncap;
        }

        memcpy(*line + *linelen, r->buf + r->pos, take);
        *linelen += take;
        (*line)[*linelen] = '\0';
        r->pos += take;

        if (nl != NULL) {
            r->pos++;                        /* пропускаємо сам '\n' */
            return 1;
        }
    }
}

/*
 * Розбір одного символу UTF-8: повертає його код і кількість байтів.
 * Потрібен для опції -i: tolower() у локалі C не змінює багатобайтові літери,
 * тому регістр обробляємо самі.
 */
static unsigned int utf8_next(const char *s, size_t n, size_t i, size_t *width)
{
    unsigned char c = (unsigned char)s[i];

    if (c < 0x80) {
        *width = 1;
        return c;
    }
    if ((c & 0xE0) == 0xC0 && i + 1 < n) {
        *width = 2;
        return (unsigned int)((c & 0x1Fu) << 6) |
               (unsigned int)((unsigned char)s[i + 1] & 0x3Fu);
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < n) {
        *width = 3;
        return (unsigned int)((c & 0x0Fu) << 12) |
               (unsigned int)(((unsigned char)s[i + 1] & 0x3Fu) << 6) |
               (unsigned int)((unsigned char)s[i + 2] & 0x3Fu);
    }
    if ((c & 0xF8) == 0xF0 && i + 3 < n) {
        *width = 4;
        return (unsigned int)((c & 0x07u) << 18) |
               (unsigned int)(((unsigned char)s[i + 1] & 0x3Fu) << 12) |
               (unsigned int)(((unsigned char)s[i + 2] & 0x3Fu) << 6) |
               (unsigned int)((unsigned char)s[i + 3] & 0x3Fu);
    }
    *width = 1;                          /* некоректний байт - беремо як є */
    return c;
}

/* Приведення до нижнього регістру: ASCII та кирилиця (А-Я, Ё). */
static unsigned int fold_case(unsigned int cp)
{
    if (cp >= 'A' && cp <= 'Z') {
        return cp + 32u;
    }
    if (cp >= 0x0410u && cp <= 0x042Fu) {
        return cp + 32u;                 /* U+0410..U+042F -> U+0430..U+044F */
    }
    if (cp == 0x0401u) {
        return 0x0451u;                  /* Ё -> ё */
    }
    return cp;
}

/* Літера, цифра або підкреслення - для опції -w. */
static int is_word_cp(unsigned int cp)
{
    if (cp < 0x80u) {
        return isalnum((int)cp) || cp == (unsigned int)'_';
    }
    return (cp >= 0x0410u && cp <= 0x044Fu) || cp == 0x0401u || cp == 0x0451u;
}

/*
 * Пошук шаблону, починаючи з байта start. Повертає довжину збігу в байтах
 * або 0, якщо не збігається. Регулярні вирази не використовуються -
 * порівняння йде символ за символом (з урахуванням UTF-8 для -i).
 */
static size_t match_at(const char *line, size_t n, size_t start,
                       const char *pat, size_t m, int icase)
{
    size_t li = start;
    size_t pi = 0;

    while (pi < m) {
        unsigned int cl;
        unsigned int cp;
        size_t wl;
        size_t wp;

        if (li >= n) {
            return 0;
        }
        cl = utf8_next(line, n, li, &wl);
        cp = utf8_next(pat, m, pi, &wp);
        if (icase) {
            cl = fold_case(cl);
            cp = fold_case(cp);
        }
        if (cl != cp) {
            return 0;
        }
        li += wl;
        pi += wp;
    }
    return li - start;
}

/* Чи є символ, розташований безпосередньо перед позицією i, «словесним»? */
static int word_before(const char *line, size_t i)
{
    size_t j = i;
    size_t w;
    unsigned int cp;

    while (j > 0 && ((unsigned char)line[j - 1] & 0xC0u) == 0x80u) {
        j--;                             /* відступаємо до початку символу */
    }
    if (j == 0) {
        return 0;
    }
    j--;
    cp = utf8_next(line, i, j, &w);
    return is_word_cp(cp);
}

static int word_after(const char *line, size_t n, size_t i)
{
    size_t w;

    if (i >= n) {
        return 0;
    }
    return is_word_cp(utf8_next(line, n, i, &w));
}

/* Перевірка, чи містить рядок шаблон (з урахуванням опцій -i та -w). */
static int line_matches(const char *line, size_t n, const char *pat, const struct opts *o)
{
    size_t m = strlen(pat);
    size_t i;

    if (m == 0) {
        return 1;                        /* порожній шаблон збігається з усім */
    }

    for (i = 0; i + m <= n; i++) {
        size_t len = match_at(line, n, i, pat, m, o->icase);

        if (len == 0) {
            continue;
        }
        if (!o->word) {
            return 1;
        }
        if (!word_before(line, i) && !word_after(line, n, i + len)) {
            return 1;
        }
    }
    return 0;
}

/* Усе виведення програми йде через write() - тоді порядок рядків не залежить
   від буферизації stdio (змішувати printf() і write() небезпечно). */
static int out_str(const char *s)
{
    return (write_all(STDOUT_FILENO, s, strlen(s)) < 0) ? -1 : 0;
}

/*
 * Опрацьовує один вхідний потік. Повертає кількість рядків зі збігами
 * або -1, якщо була помилка читання.
 */
static long grep_fd(int fd, const char *name, const char *pat,
                    const struct opts *o, int show_name)
{
    struct reader r;
    char *line = NULL;
    size_t linelen = 0;
    size_t linecap = 0;
    off_t offset = 0;
    long lineno = 0;
    long hits = 0;
    int rc;

    if (reader_init(&r, fd) < 0) {
        warn_at("не вдалося виділити буфер читання", name);
        return -1;
    }

    while ((rc = reader_line(&r, &line, &linelen, &linecap, &offset)) > 0) {
        char prefix[512];
        int plen = 0;
        int match;

        prefix[0] = '\0';        /* без опцій -n/-b префікс порожній */

        lineno++;
        match = line_matches(line, linelen, pat, o);
        if (o->invert) {
            match = !match;
        }
        if (!match) {
            continue;
        }
        hits++;
        if (o->quiet) {
            break;                           /* -q: досить першого збігу */
        }
        if (o->count_only) {
            continue;
        }
        if (r.saw_nul) {
            /* Двійковий файл: як справжній grep, вміст рядка не друкуємо. */
            snprintf(prefix, sizeof prefix, "Двійковий файл %s збігається з шаблоном\n",
                     name);
            out_str(prefix);
            break;
        }
        if (show_name) {
            plen += snprintf(prefix + plen, sizeof prefix - (size_t)plen, "%s:", name);
        }
        if (o->number) {
            plen += snprintf(prefix + plen, sizeof prefix - (size_t)plen, "%ld:", lineno);
        }
        if (o->byte_off) {
            plen += snprintf(prefix + plen, sizeof prefix - (size_t)plen, "%lld:",
                             (long long)offset);
        }
        /* Префікс і сам рядок пишемо однаковим способом - через write().
           Якщо змішати printf() і write(), буферизація stdio переставить
           місцями номер рядка та текст. */
        if (out_str(prefix) < 0 || write_all(STDOUT_FILENO, line, linelen) < 0 ||
            out_str("\n") < 0) {
            warn_at("помилка запису у стандартний вивід", NULL);
            free(line);
            reader_free(&r);
            return -1;
        }
    }

    if (rc < 0) {
        if (!o->silent) {
            warn_at("помилка читання", name);
        }
        free(line);
        reader_free(&r);
        return -1;
    }

    if (o->count_only && !o->quiet) {
        char buf[512];

        if (show_name) {
            snprintf(buf, sizeof buf, "%s:%ld\n", name, hits);
        } else {
            snprintf(buf, sizeof buf, "%ld\n", hits);
        }
        out_str(buf);
    }

    free(line);
    reader_free(&r);
    return hits;
}

int main(int argc, char *argv[])
{
    struct opts o = { 0, 0, 0, 0, 0, 0, 0, 0 };
    int opt;
    const char *pat;
    int nfiles;
    int show_name;
    int any_match = 0;
    int had_error = 0;
    int i;

    prog_init(argv[0]);

    while ((opt = getopt(argc, argv, "ivcnqswbh")) != -1) {
        switch (opt) {
        case 'i':
            o.icase = 1;
            break;
        case 'v':
            o.invert = 1;
            break;
        case 'c':
            o.count_only = 1;
            break;
        case 'n':
            o.number = 1;
            break;
        case 'q':
            o.quiet = 1;
            break;
        case 's':
            o.silent = 1;
            break;
        case 'w':
            o.word = 1;
            break;
        case 'b':
            o.byte_off = 1;
            break;
        case 'h':
            usage();
            return EXIT_SUCCESS;
        default:
            usage();
            return 2;
        }
    }

    if (optind >= argc) {
        err_msg("не вказано шаблон пошуку");
        usage();
        return 2;
    }
    pat = argv[optind++];
    nfiles = argc - optind;
    show_name = (nfiles > 1) ? 1 : 0;

    if (nfiles == 0) {
        long hits = grep_fd(STDIN_FILENO, "(стандартний вхід)", pat, &o, 0);

        if (hits < 0) {
            return 2;
        }
        return (hits > 0) ? 0 : 1;
    }

    for (i = optind; i < argc; i++) {
        int fd;
        long hits;

        if (strcmp(argv[i], "-") == 0) {
            fd = STDIN_FILENO;               /* '-' - читати зі стандартного входу */
        } else {
            fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                if (!o.silent) {
                    warn_at("не вдалося відкрити", argv[i]);
                }
                had_error = 1;
                continue;
            }
        }

        hits = grep_fd(fd, argv[i], pat, &o, show_name);
        if (hits < 0) {
            had_error = 1;
        } else if (hits > 0) {
            any_match = 1;
        }
        if (fd != STDIN_FILENO) {
            if (close(fd) < 0) {
                warn_at("close", argv[i]);
                had_error = 1;
            }
        }
    }

    /* Коди завершення, як у справжньому grep: 0 - знайдено, 1 - не знайдено,
       2 - помилка (наприклад, файл не відкрився). */
    if (had_error) {
        return 2;
    }
    return any_match ? 0 : 1;
}
