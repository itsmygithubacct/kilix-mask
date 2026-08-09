PROJECT := kilix-mask
BUILD_DIR ?= build
PREFIX ?= /usr/local
DESTDIR ?=

CC ?= cc
AR ?= ar
INSTALL ?= install

CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Iinclude
WARNINGS := \
	-Wall -Wextra -Wpedantic -Wconversion -Wshadow \
	-Wstrict-prototypes -Wmissing-prototypes -Wformat=2
CFLAGS ?= -O2 -g
override CFLAGS += -std=c11 -fPIC $(WARNINGS)
LDLIBS += -lz

STATIC_LIB := $(BUILD_DIR)/lib$(PROJECT).a
SHARED_LIB := $(BUILD_DIR)/lib$(PROJECT).so
OBJECTS := $(BUILD_DIR)/kilix_mask.o $(BUILD_DIR)/kilix_mask_rects.o

TEST_SOURCES := $(wildcard tests/test_*.c)
TEST_BINARIES := $(patsubst tests/test_%.c,$(BUILD_DIR)/test-%,$(TEST_SOURCES))

.PHONY: all test sanitize install clean

all: $(STATIC_LIB) $(SHARED_LIB)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: src/%.c include/kilix_mask.h include/kilix_mask_rects.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(STATIC_LIB): $(OBJECTS)
	$(AR) rcs $@ $^

$(SHARED_LIB): $(OBJECTS)
	$(CC) -shared $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/test-%: tests/test_%.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(STATIC_LIB) $(LDLIBS) -o $@

test: $(TEST_BINARIES)
	@set -e; for binary in $(TEST_BINARIES); do \
		printf '\n== %s ==\n' "$$binary"; \
		"$$binary"; \
	done; \
	printf '\nall test suites passed\n'

sanitize: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
sanitize: LDFLAGS += -fsanitize=address,undefined
sanitize: clean
	@$(MAKE) --no-print-directory \
		CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)" test

install: all
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/include
	$(INSTALL) -m 644 include/kilix_mask.h include/kilix_mask_rects.h \
		$(DESTDIR)$(PREFIX)/include/
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/lib
	$(INSTALL) -m 644 $(STATIC_LIB) $(DESTDIR)$(PREFIX)/lib/
	$(INSTALL) -m 755 $(SHARED_LIB) $(DESTDIR)$(PREFIX)/lib/

clean:
	rm -rf $(BUILD_DIR)
