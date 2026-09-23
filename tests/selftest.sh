#!/usr/bin/env bash
# selftest.sh - самоперевірка утиліт: результати порівнюються з системними
# cp / grep / find. Кожна перевірка друкує PASS або FAIL, у кінці - підсумок.
set -u

cd "$(dirname "$0")/.." || exit 1
TMP="tests/.selftest_tmp"
rm -rf "$TMP"
mkdir -p "$TMP"
PASS=0
FAIL=0

ok()   { PASS=$((PASS + 1)); printf 'PASS  %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf 'FAIL  %s\n' "$1"; }

check() { # check "назва" очікуваний_код фактичний_код
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (очікували $2, отримали $3)"; fi
}

# ---------- mycp ----------
head -c 300000 /dev/urandom > "$TMP/random.bin"
./mycp "$TMP/random.bin" "$TMP/copy.bin"
if cmp -s "$TMP/random.bin" "$TMP/copy.bin"; then
    ok "mycp: копія побайтово збігається з оригіналом (cmp)"
else
    bad "mycp: копія відрізняється від оригіналу"
fi

# Порівняння з системним cp: обидва результати мають бути ідентичні джерелу.
cp "$TMP/random.bin" "$TMP/sys.bin"
cmp -s "$TMP/copy.bin" "$TMP/sys.bin" && ok "mycp: результат дорівнює cp" \
    || bad "mycp: результат відрізняється від cp"

# Буфер нестандартного розміру (7 байтів) - перевірка циклу часткових записів.
./mycp -b 7 "$TMP/random.bin" "$TMP/copy7.bin"
cmp -s "$TMP/random.bin" "$TMP/copy7.bin" && ok "mycp: буфер 7 байтів, файл цілий" \
    || bad "mycp: копіювання малим буфером зіпсувало файл"

# Розріджений файл: -S має зберегти «дірку» (менший st_blocks при тому ж розмірі).
truncate -s 4M "$TMP/sparse.bin"
printf 'дані' | dd of="$TMP/sparse.bin" bs=1 seek=0 conv=notrunc status=none
./mycp -S "$TMP/sparse.bin" "$TMP/sparse_copy.bin"
if [ "$(stat -c %s "$TMP/sparse_copy.bin")" = "$(stat -c %s "$TMP/sparse.bin")" ]; then
    ok "mycp -S: розмір файлу збережено"
else
    bad "mycp -S: розмір файлу змінився"
fi
if [ "$(stat -c %b "$TMP/sparse_copy.bin")" -le "$(stat -c %b "$TMP/sparse.bin")" ]; then
    ok "mycp -S: дірка не заповнена нулями (блоків $(( $(stat -c %b "$TMP/sparse_copy.bin") )) )"
else
    bad "mycp -S: копія зайняла більше блоків, ніж оригінал"
fi

# Опція -n: наявний файл не перезаписується.
echo "старий вміст" > "$TMP/exists.txt"
echo "новий вміст" > "$TMP/new.txt"
./mycp -n "$TMP/new.txt" "$TMP/exists.txt" 2> "$TMP/n_err.txt"
check "mycp -n: код завершення 1 (файл не перезаписано)" 1 $?
grep -q "старий вміст" "$TMP/exists.txt" && ok "mycp -n: вміст збережено" \
    || bad "mycp -n: вміст перезаписано"
grep -q "File exists" "$TMP/n_err.txt" && ok "mycp -n: повідомлення містить strerror (File exists)" \
    || bad "mycp -n: у повідомленні немає тексту помилки"

# Опція -p: права доступу зберігаються.
chmod 640 "$TMP/random.bin"
./mycp -p "$TMP/random.bin" "$TMP/perm.bin"
check "mycp -p: права доступу збережено (640)" "640" "$(stat -c %a "$TMP/perm.bin")"

# Помилка: джерела не існує.
./mycp "$TMP/немає.txt" "$TMP/x.txt" 2> "$TMP/e1.txt"
check "mycp: код завершення 1 для неіснуючого джерела" 1 $?
grep -q "No such file or directory" "$TMP/e1.txt" \
    && ok "mycp: errno=ENOENT пояснено через strerror" \
    || bad "mycp: немає пояснення errno"

# Помилка: каталог як джерело без рекурсії.
mkdir -p "$TMP/dir_src"
./mycp "$TMP/dir_src" "$TMP/dir_copy" 2> "$TMP/e2.txt"
check "mycp: код завершення 1 для каталогу-джерела" 1 $?

# ---------- mygrep ----------
printf 'яблуко\nБанан\nяблучний сік\nвишня\nГруша\n' > "$TMP/fruit.txt"

./mygrep -n "яблу" "$TMP/fruit.txt" > "$TMP/g1.txt"; rc=$?
grep -n "яблу" "$TMP/fruit.txt" > "$TMP/g1_ref.txt"
check "mygrep -n: код завершення 0" 0 "$rc"
diff -q "$TMP/g1.txt" "$TMP/g1_ref.txt" > /dev/null \
    && ok "mygrep -n: вивід збігається з системним grep" \
    || bad "mygrep -n: вивід відрізняється від grep"

./mygrep -i "банан" "$TMP/fruit.txt" > "$TMP/g2.txt"
grep -i "банан" "$TMP/fruit.txt" > "$TMP/g2_ref.txt"
diff -q "$TMP/g2.txt" "$TMP/g2_ref.txt" > /dev/null \
    && ok "mygrep -i: вивід збігається з grep -i" \
    || bad "mygrep -i: вивід відрізняється"

./mygrep -v "яблу" "$TMP/fruit.txt" > "$TMP/g3.txt"
grep -v "яблу" "$TMP/fruit.txt" > "$TMP/g3_ref.txt"
diff -q "$TMP/g3.txt" "$TMP/g3_ref.txt" > /dev/null \
    && ok "mygrep -v: вивід збігається з grep -v" \
    || bad "mygrep -v: вивід відрізняється"

check "mygrep -c: кількість збігів = 2" "2" "$(./mygrep -c "яблу" "$TMP/fruit.txt")"
./mygrep "немаєтакого" "$TMP/fruit.txt" > /dev/null
check "mygrep: код завершення 1, коли збігів немає" 1 $?
./mygrep "яблу" "$TMP/немає.txt" 2> /dev/null
check "mygrep: код завершення 2 при помилці відкриття" 2 $?

# Рядок довший за буфер читання (100000 байтів) - перевірка динамічного буфера.
python3 -c "print('a'*100000 + 'ШУКАЙ')" > "$TMP/long.txt"
./mygrep -c "ШУКАЙ" "$TMP/long.txt" > "$TMP/g4.txt"
check "mygrep: рядок 100000 байтів знайдено" "1" "$(cat "$TMP/g4.txt")"

# Читання зі стандартного входу (канал): lseek недоступний, але працює.
printf 'один\nдва\nтри\n' | ./mygrep -c "д" > "$TMP/g5.txt"
check "mygrep: читання з каналу (stdin)" "2" "$(cat "$TMP/g5.txt")"

# Зміщення у байтах через lseek (-b) - як у справжнього grep.
./mygrep -b "вишня" "$TMP/fruit.txt" > "$TMP/g6.txt"
grep -b "вишня" "$TMP/fruit.txt" > "$TMP/g6_ref.txt"
diff -q "$TMP/g6.txt" "$TMP/g6_ref.txt" > /dev/null \
    && ok "mygrep -b: зміщення збігається з grep -b" \
    || bad "mygrep -b: зміщення відрізняється"

# -w: збіг лише як окреме слово.
printf 'cat\ncategory\nthe cat\n' > "$TMP/words.txt"
check "mygrep -w: 2 рядки з окремим словом cat" "2" "$(./mygrep -wc "cat" "$TMP/words.txt")"

# ---------- mytree ----------
if [ "$(./mytree -m "$TMP" 2>/dev/null | grep -c '^-')" -gt 0 ]; then
    ok "mytree: у плоскому списку є звичайні файли"
else
    bad "mytree: плоский список порожній"
fi

# Кількість файлів має збігатися з find (той самий обхід дерева, але бібліотечний).
n_find=$(find "$TMP" -type f | wc -l)
n_my=$(./mytree -m "$TMP" 2>/dev/null | grep -c '^-')
if [ "$n_find" = "$n_my" ]; then
    ok "mytree: кількість файлів збігається з find ($n_find)"
else
    bad "mytree: файлів за find $n_find, за mytree $n_my"
fi

# Метадані у виводі мають відповідати stat для конкретного файлу.
my_line=$(./mytree -b -m "$TMP/fruit.txt" 2>/dev/null | tail -n 1)
case "$my_line" in
    "$(stat -c %A "$TMP/fruit.txt")"*"$(stat -c %s "$TMP/fruit.txt")"*"$TMP/fruit.txt")
        ok "mytree: права та розмір у виводі збігаються зі stat" ;;
    *)
        bad "mytree: метадані не збігаються зі stat ($my_line)" ;;
esac

# Час останньої модифікації: порівнюємо з показом stat у тому ж форматі.
if ./mytree -m "$TMP/fruit.txt" 2>/dev/null | grep -q "$(stat -c %y "$TMP/fruit.txt" | cut -c1-19)"; then
    ok "mytree: час модифікації збігається зі stat"
else
    bad "mytree: час модифікації не збігається"
fi

# Символьне посилання показується разом із ціллю.
ln -sf "fruit.txt" "$TMP/link.txt"
./mytree "$TMP" 2>/dev/null | grep -q 'link.txt -> fruit.txt' \
    && ok "mytree: символьне посилання і його ціль" \
    || bad "mytree: посилання не показано"

# Помилка доступу до каталогу не зупиняє обхід (errno=EACCES).
mkdir -p "$TMP/noaccess/sub"
touch "$TMP/noaccess/sub/файл.txt"
chmod 000 "$TMP/noaccess/sub"
./mytree "$TMP/noaccess" > /dev/null 2> "$TMP/w_err.txt"
rc=$?
chmod 755 "$TMP/noaccess/sub"
grep -q "Permission denied" "$TMP/w_err.txt" \
    && ok "mytree: errno=EACCES пояснено через strerror" \
    || bad "mytree: немає пояснення Permission denied"
check "mytree: код завершення 1 після помилки доступу" 1 "$rc"

# Неіснуючий шлях.
./mytree "$TMP/немає_такого" > /dev/null 2>&1
check "mytree: код завершення 1 для неіснуючого шляху" 1 $?

printf '\nПідсумок самоперевірки: PASS=%d FAIL=%d\n' "$PASS" "$FAIL"
rm -rf "$TMP"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
