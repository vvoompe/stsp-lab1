/*
 * common.h - спільні допоміжні засоби для утиліт mycp, mygrep, mytree.
 *
 * Тут зібрано те, що повторюється у всіх трьох програмах:
 *   - повідомлення про помилки у стилі perror(), але з поясненням через errno
 *     та strerror() і з назвою об'єкта, на якому сталась помилка;
 *   - повний запис буфера, бо write() має право записати менше байтів, ніж просили;
 *   - форматування розміру, прав доступу та часу останньої модифікації.
 *
 * Реалізація - у common.c, щоб не тягнути дублікати в кожен виконуваний файл.
 */
#ifndef MYCOMMON_H
#define MYCOMMON_H

#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Назва утиліти для префікса повідомлень ("mycp: ...").
   Заповнюється з argv[0] на старті main(). */
extern const char *g_prog;
void prog_init(const char *argv0);

/* <prog>: <що робили> '<об'єкт>': <текст помилки з errno> - як у perror().
   errno зберігається у локальній змінній, бо fprintf() може його перезаписати. */
void warn_at(const char *what, const char *name);

/* Те саме, але після повідомлення програма завершується з кодом EXIT_FAILURE. */
void die_at(const char *what, const char *name);

/* Діагностика, не пов'язана з errno (неправильні аргументи тощо). */
void err_msg(const char *fmt, ...);

/* Записує рівно count байтів: дописує часткові записи, повторює при EINTR.
   Повертає count або -1 (errno встановлено). */
ssize_t write_all(int fd, const void *buf, size_t count);

/* read() з повторенням при EINTR. */
ssize_t read_retry(int fd, void *buf, size_t count);

/* "1.5 МіБ" / "512 Б" - для людиночитних звітів. */
void human_size(off_t size, char *out, size_t outsz);

/* "drwxr-xr-x" - права доступу у вигляді, як у ls -l. out має бути >= 11 байтів. */
void mode_string(mode_t mode, char out[11]);

/* Час останньої модифікації у локальному часі: "2026-09-23 13:31:04". */
void time_string(time_t t, char *out, size_t outsz);

#endif /* MYCOMMON_H */
