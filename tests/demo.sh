#!/usr/bin/env bash
# demo.sh - демонстраційний прогін для захисту лабораторної роботи.
# Створює тестове дерево у теці-пісочниці й показує можливості mycp, mygrep, mytree,
# а також навмисні помилки, щоб було видно обробку errno через strerror().
#
# Запуск:  make demo
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SANDBOX="$ROOT/tests/.demo_tmp"

banner() {
    printf '\n$ %s\n' "$1"
    printf -- '----------------------------------------------------------------\n'
}

rm -rf "$SANDBOX"
mkdir -p "$SANDBOX"
cd "$SANDBOX" || exit 1

# ---------- 0. Підготовка тестового дерева ----------
mkdir -p demo/docs demo/src/sub demo/data demo/links copy

printf 'Звіт про роботу утиліт.\nДругий рядок.\nТретій рядок із словом kernel.\n' \
    > demo/docs/readme.txt
printf 'нотатки\n' > demo/docs/.notes
printf '#include <stdio.h>\nint main(void) { return 0; }\n' > demo/src/main.c
printf 'допоміжний модуль\n' > demo/src/util.c
printf 'глибокий файл\n' > demo/src/sub/deep.c
head -c 300000 /dev/urandom > demo/data/random.bin
truncate -s 8M demo/data/sparse.img                       # розріджений файл (дірка)
printf 'MAGIC' | dd of=demo/data/sparse.img bs=1 seek=0 conv=notrunc status=none
printf 'журнал\nkernel: усі повідомлення\nkernel: ще один рядок\n' > demo/data/kernel.log
chmod 400 demo/data/kernel.log
ln -sf ../docs/readme.txt demo/links/readme.link
printf 'ЯБЛУКО\nяблуко\nБанан\n' > demo/data/fruit.txt

banner "find demo -printf '%M %8s %p'"
find demo -printf '%M %8s %p\n'

banner "stat -c 'розмір=%s блоків=%b' demo/data/sparse.img"
stat -c 'розмір=%s байтів (блоків %b по 512 Б)' demo/data/sparse.img

# ---------- 1-2. mycp: копіювання ----------
banner "mycp -v demo/data/random.bin copy/"
"$ROOT/mycp" -v demo/data/random.bin copy/
cmp demo/data/random.bin copy/random.bin && echo "cmp: копія побайтово ідентична джерелу"

banner "mycp -v -b 512 demo/data/random.bin copy/random_512.bin"
"$ROOT/mycp" -v -b 512 demo/data/random.bin copy/random_512.bin
cmp demo/data/random.bin copy/random_512.bin && echo "cmp: файл цілий і з буфером 512 байтів"

# ---------- 3. Розріджений файл ----------
banner "mycp -v -S demo/data/sparse.img copy/sparse_mycp.img"
"$ROOT/mycp" -v -S demo/data/sparse.img copy/sparse_mycp.img
cp --sparse=always demo/data/sparse.img copy/sparse_syscp.img
printf '%-12s ' "джерело"; stat -c 'розмір=%s, блоків=%b' demo/data/sparse.img
printf '%-12s ' "mycp -S"; stat -c 'розмір=%s, блоків=%b' copy/sparse_mycp.img
printf '%-12s ' "cp --sparse"; stat -c 'розмір=%s, блоків=%b' copy/sparse_syscp.img

# ---------- 4. Збереження метаданих ----------
banner "mycp -v -p demo/data/kernel.log copy/kernel.log"
"$ROOT/mycp" -v -p demo/data/kernel.log copy/kernel.log
printf 'джерело: %s\nкопія  : %s\n' \
    "$(stat -c '%A %y' demo/data/kernel.log)" "$(stat -c '%A %y' copy/kernel.log)"

# ---------- 5. Обробка помилок ----------
banner "mycp: навмисні помилки та коди завершення"
"$ROOT/mycp" demo/data/немає_такого copy/x; echo "код завершення: $?"
"$ROOT/mycp" demo/src copy/src; echo "код завершення: $?"
"$ROOT/mycp" -n demo/data/kernel.log copy/kernel.log; echo "код завершення: $?"
"$ROOT/mycp" demo/data/random.bin немає_каталогу/y; echo "код завершення: $?"

# ---------- 6-9. mygrep ----------
banner "mygrep -n kernel demo/data/kernel.log"
"$ROOT/mygrep" -n kernel demo/data/kernel.log

banner "diff <(mygrep -n kernel ...) <(grep -n kernel ...)"
"$ROOT/mygrep" -n kernel demo/data/kernel.log > my_out.txt
grep -n kernel demo/data/kernel.log > sys_out.txt
diff my_out.txt sys_out.txt && echo "diff: вивід mygrep і системного grep збігається повністю"

banner "mygrep -i / -v / -c / -b"
echo "--- -in яблуко (регістр літер не враховується)"
"$ROOT/mygrep" -in яблуко demo/data/fruit.txt
echo "--- -vn kernel (рядки, де шаблону немає)"
"$ROOT/mygrep" -vn kernel demo/data/kernel.log
echo "--- -wc kernel (лише кількість рядків зі збігом)"
"$ROOT/mygrep" -wc kernel demo/data/kernel.log
echo "--- -b kernel (зміщення першого байта рядка)"
"$ROOT/mygrep" -b kernel demo/data/kernel.log

banner "mygrep kernel demo/docs/readme.txt demo/data/kernel.log"
"$ROOT/mygrep" kernel demo/docs/readme.txt demo/data/kernel.log
echo "код завершення при знайденому збігу: $?"
"$ROOT/mygrep" неіснуючий_рядок demo/data/kernel.log
echo "код завершення без збігів: $?"
printf 'kernel через канал\n' | "$ROOT/mygrep" -c kernel -
"$ROOT/mygrep" kernel немає_файлу; echo "код завершення при помилці: $?"

# ---------- 10-13. mytree ----------
banner "mytree demo"
"$ROOT/mytree" demo

banner "mytree -a -d 2 demo"
"$ROOT/mytree" -a -d 2 demo

banner "mytree -m -b demo/data"
"$ROOT/mytree" -m -b demo/data

banner "mytree demo/secret (каталог без прав доступу)"
mkdir -p demo/secret/sub
printf 'таємний файл\n' > demo/secret/sub/hidden.txt
chmod 000 demo/secret/sub
"$ROOT/mytree" demo/secret; echo "код завершення: $?"
chmod 755 demo/secret/sub

banner "mytree немає_каталогу"
"$ROOT/mytree" немає_каталогу; echo "код завершення: $?"

printf '\nТестові файли залишено у %s (видалення: make clean)\n' "$SANDBOX"
