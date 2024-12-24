#!/usr/bin/env make
CFLAGS += -Wall -Wextra
LDFLAGS += -lX11 -lXfixes -lXdamage -lXcomposite -lXrender -lXext -linput -ludev -levdev
-include .makerc

csrc := $(wildcard src/*.c) $(wildcard src/**/*.c)

obj := $(csrc:%.c=obj/%.o)
obj/%.o: %.c
	@mkdir -p "$(@D)"
	@$(CC) -c $(CFLAGS) $< -o $@ $(LDFLAGS)

bin/mgnfx: $(obj)
	@mkdir -p bin
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

.PHONY: clean
clean:
	$(RM) -r obj
	$(RM) -r bin

.DEFAULT_GOAL = bin/mgnfx
