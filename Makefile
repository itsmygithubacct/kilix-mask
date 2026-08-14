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

# Vendored and pinned.  Only the editor needs soft-raster; the model and
# the rectangle decomposition depend on nothing but C11 and zlib, which is
# why they are a separate library.  The terminal stack below that is
# needed only by the command.
SR := third_party/soft-raster
KTS := third_party/kitty-terminal-session
KFB := $(KTS)/third_party/kitty-framebuffer
KIN := $(KTS)/third_party/kitty-input
KKB := $(KIN)/third_party/kitty_keyboard

EDIT_CPPFLAGS := -I$(SR)/include
EDIT_LDLIBS := -lm

CMD_CPPFLAGS := $(EDIT_CPPFLAGS) -I$(KTS)/include -I$(KFB)/include \
	-I$(KIN)/include -I$(KKB)/include -Isrc
CMD_LDLIBS := -lm -lpthread

CMD_SOURCES := src/main.c src/kmask_run.c src/kmask_ui.c \
	src/kmask_marks.c
CMD_VENDOR_SOURCES := \
	$(KTS)/src/kitty_terminal_session.c \
	$(KFB)/src/kitty_framebuffer.c \
	$(KIN)/src/kitty_input.c \
	$(KIN)/src/kitty_input_posix.c \
	$(KKB)/src/kitty_keyboard.c \
	$(KKB)/src/kitty_keyboard_posix.c
CMD_VENDOR_OBJECTS := \
	$(patsubst %.c,$(BUILD_DIR)/vendor/%.o,$(notdir $(CMD_VENDOR_SOURCES)))
COMMAND := $(BUILD_DIR)/kilix-mask

STATIC_LIB := $(BUILD_DIR)/lib$(PROJECT).a
SHARED_LIB := $(BUILD_DIR)/lib$(PROJECT).so
OBJECTS := $(BUILD_DIR)/kilix_mask.o $(BUILD_DIR)/kilix_mask_rects.o

EDIT_LIB := $(BUILD_DIR)/lib$(PROJECT)-edit.a
EDIT_OBJECTS := $(BUILD_DIR)/kilix_mask_edit.o \
	$(BUILD_DIR)/kilix_mask_image.o

# Pinned upstream code, built with the conversion warnings off: letting
# their output through would bury ours.
SR_OBJECT := $(BUILD_DIR)/vendor/soft_raster.o
VENDOR_CFLAGS := $(CFLAGS) -Wno-conversion -Wno-sign-conversion

TESTS := $(BUILD_DIR)/test-mask $(BUILD_DIR)/test-rects \
	$(BUILD_DIR)/test-edit $(BUILD_DIR)/test-image $(BUILD_DIR)/test-marks

BENCH := $(BUILD_DIR)/bench-rects

.PHONY: all test benchmark sanitize install clean

all: $(STATIC_LIB) $(SHARED_LIB) $(EDIT_LIB) $(COMMAND)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/vendor:
	mkdir -p $(BUILD_DIR)/vendor

$(BUILD_DIR)/%.o: src/%.c include/kilix_mask.h include/kilix_mask_rects.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(STATIC_LIB): $(OBJECTS)
	$(AR) rcs $@ $^

$(SHARED_LIB): $(OBJECTS)
	$(CC) -shared $(LDFLAGS) $^ $(LDLIBS) -o $@

$(BUILD_DIR)/kilix_mask_edit.o: src/kilix_mask_edit.c include/kilix_mask_edit.h \
		include/kilix_mask.h | $(BUILD_DIR)
	@test -f $(SR)/src/soft_raster.c || { \
		printf 'submodules missing; run: git submodule update --init --recursive\n' >&2; \
		exit 1; }
	$(CC) $(CPPFLAGS) $(EDIT_CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/kilix_mask_image.o: src/kilix_mask_image.c \
		include/kilix_mask_image.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(EDIT_CPPFLAGS) $(CFLAGS) -c $< -o $@

# Not bundled into the archive: a consumer that already links soft-raster
# would get every symbol twice.
$(EDIT_LIB): $(EDIT_OBJECTS)
	$(AR) rcs $@ $^

$(SR_OBJECT): $(SR)/src/soft_raster.c | $(BUILD_DIR)/vendor
	$(CC) $(CPPFLAGS) $(EDIT_CPPFLAGS) $(VENDOR_CFLAGS) -c $< -o $@

# vpath lets one rule cover vendored sources from several trees.
vpath %.c $(sort $(dir $(CMD_VENDOR_SOURCES)))

$(BUILD_DIR)/vendor/%.o: %.c | $(BUILD_DIR)/vendor
	$(CC) $(CPPFLAGS) $(CMD_CPPFLAGS) $(VENDOR_CFLAGS) -c $< -o $@

$(COMMAND): $(CMD_SOURCES) $(CMD_VENDOR_OBJECTS) $(EDIT_LIB) $(STATIC_LIB) \
		$(SR_OBJECT) | $(BUILD_DIR)
	@test -f $(KTS)/src/kitty_terminal_session.c || { \
		printf 'submodules missing; run: git submodule update --init --recursive\n' >&2; \
		exit 1; }
	$(CC) $(CPPFLAGS) $(CMD_CPPFLAGS) $(CFLAGS) $(LDFLAGS) \
		$(CMD_SOURCES) $(CMD_VENDOR_OBJECTS) $(EDIT_LIB) $(STATIC_LIB) \
		$(SR_OBJECT) $(LDLIBS) $(CMD_LDLIBS) -o $@

$(BUILD_DIR)/test-%: tests/test_%.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(STATIC_LIB) $(LDLIBS) -o $@

$(BUILD_DIR)/test-edit: tests/test_edit.c $(EDIT_LIB) $(STATIC_LIB) \
		$(SR_OBJECT) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(EDIT_CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< \
		$(EDIT_LIB) $(STATIC_LIB) $(SR_OBJECT) $(LDLIBS) $(EDIT_LDLIBS) -o $@

# Marks belong to the command rather than either library, so the test
# compiles that source directly.
$(BUILD_DIR)/test-marks: tests/test_marks.c src/kmask_marks.c \
		$(EDIT_LIB) $(STATIC_LIB) $(SR_OBJECT) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(EDIT_CPPFLAGS) -Isrc $(CFLAGS) $(LDFLAGS) \
		tests/test_marks.c src/kmask_marks.c \
		$(EDIT_LIB) $(STATIC_LIB) $(SR_OBJECT) $(LDLIBS) $(EDIT_LDLIBS) -o $@

$(BUILD_DIR)/test-image: tests/test_image.c tests/image_fixtures.h \
		$(EDIT_LIB) $(STATIC_LIB) $(SR_OBJECT) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(EDIT_CPPFLAGS) -Itests $(CFLAGS) $(LDFLAGS) $< \
		$(EDIT_LIB) $(STATIC_LIB) $(SR_OBJECT) $(LDLIBS) $(EDIT_LDLIBS) -o $@

$(BENCH): benchmarks/bench_rects.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(STATIC_LIB) $(LDLIBS) -o $@

benchmark: $(BENCH)
	$(BENCH)

# The command's own --selftest runs here too.  It duplicates a little of
# what the suites cover, deliberately: it is the only check that the
# assembled binary works, and it is what can be run on a machine that has
# the tool installed but not this source tree.  The benchmark runs with
# the suites because its floor is a functional contract: a decomposition
# that slows down by orders of magnitude does not fail anything at run
# time, it just quietly stops the rectangle count refreshing.
test: $(TESTS) $(COMMAND) $(BENCH)
	@set -e; for binary in $(TESTS); do \
		printf '\n== %s ==\n' "$$binary"; \
		"$$binary"; \
	done; \
	printf '\n== %s --selftest ==\n' "$(COMMAND)"; \
	$(COMMAND) --selftest; \
	printf '\n== %s ==\n' "$(BENCH)"; \
	$(BENCH); \
	printf '\nall test suites passed\n'

sanitize: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer
sanitize: LDFLAGS += -fsanitize=address,undefined
sanitize: clean
	@$(MAKE) --no-print-directory \
		CFLAGS="$(CFLAGS)" LDFLAGS="$(LDFLAGS)" test

install: all
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/include
	$(INSTALL) -m 644 include/kilix_mask.h include/kilix_mask_rects.h \
		include/kilix_mask_edit.h include/kilix_mask_image.h \
		$(DESTDIR)$(PREFIX)/include/
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/lib
	$(INSTALL) -m 644 $(STATIC_LIB) $(EDIT_LIB) $(DESTDIR)$(PREFIX)/lib/
	$(INSTALL) -m 755 $(SHARED_LIB) $(DESTDIR)$(PREFIX)/lib/
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m 755 $(COMMAND) $(DESTDIR)$(PREFIX)/bin/

clean:
	rm -rf $(BUILD_DIR)
