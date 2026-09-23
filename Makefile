# Makefile - збірка утиліт лабораторної роботи № 1 (СТСП)
#
# Цілі: all (типово), test (самоперевірка), demo (показ для захисту), clean.

CC      ?= gcc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -O2
LDFLAGS ?=

BIN  = mycp mygrep mytree
SRC  = src/common.c
HDR  = src/common.h

all: $(BIN)

mycp: src/mycp.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ src/mycp.c $(SRC) $(LDFLAGS)

mygrep: src/mygrep.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ src/mygrep.c $(SRC) $(LDFLAGS)

mytree: src/mytree.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ src/mytree.c $(SRC) $(LDFLAGS)

# Самоперевірка: результати mycp/mygrep/mytree порівнюються з системними
# утилітами cp/grep/find (побайтове порівняння через cmp і diff).
test: all
	bash tests/selftest.sh

# Демонстраційний прогін із заздалегідь підготовленими файлами.
demo: all
	bash tests/demo.sh

clean:
	rm -f $(BIN)
	rm -rf tests/.demo_tmp

.PHONY: all test demo clean
