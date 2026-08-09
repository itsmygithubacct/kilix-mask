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

# Vendored and pinned.  Only the editor needs it; the model and the
# rectangle decomposition depend on nothing but C11 and zlib, which is why
# they are a separate library.
SR := third_party/soft-raster
EDIT_CPPFLAGS := -I$(SR)/include
EDIT_LDLIBS := -lm

STATIC_LIB := $(BUILD_DIR)/lib$(PROJECT).a
SHARED_LIB := $(BUILD_DIR)/lib$(PROJECT).so
OBJECTS := $(BUILD_DIR)/kilix_mask.o $(BUILD_DIR)/kilix_mask_rects.o

EDIT_LIB := $(BUILD_DIR)/lib$(PROJECT)-edit.a
EDIT_OBJECTS := $(BUILD_DIR)/kilix_mask_edit.o

# Pinned upstream code, built with the conversion warnings off: letting
# their output through would bury ours.
SR_OBJECT := $(BUILD_DIR)/vendor/soft_raster.o
VENDOR_CFLAGS := $(CFLAGS) -Wno-conversion -Wno-sign-conversion

TESTS := $(BUILD_DIR)/test-mask $(BUILD_DIR)/test-rects $(BUILD_DIR)/test-edit

.PHONY: all test sanitize install clean

all: $(STATIC_LIB) $(SHARED_LIB) $(EDIT_LIB)

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

# Not bundled into the archive: a consumer that already links soft-raster
# would get every symbol twice.
$(EDIT_LIB): $(EDIT_OBJECTS)
	$(AR) rcs $@ $^

$(SR_OBJECT): $(SR)/src/soft_raster.c | $(BUILD_DIR)/vendor
	$(CC) $(CPPFLAGS) $(EDIT_CPPFLAGS) $(VENDOR_CFLAGS) -c $< -o $@

$(BUILD_DIR)/test-%: tests/test_%.c $(STATIC_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(STATIC_LIB) $(LDLIBS) -o $@

$(BUILD_DIR)/test-edit: tests/test_edit.c $(EDIT_LIB) $(STATIC_LIB) \
		$(SR_OBJECT) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(EDIT_CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< \
		$(EDIT_LIB) $(STATIC_LIB) $(SR_OBJECT) $(LDLIBS) $(EDIT_LDLIBS) -o $@

test: $(TESTS)
	@set -e; for binary in $(TESTS); do \
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
		include/kilix_mask_edit.h $(DESTDIR)$(PREFIX)/include/
	$(INSTALL) -d $(DESTDIR)$(PREFIX)/lib
	$(INSTALL) -m 644 $(STATIC_LIB) $(EDIT_LIB) $(DESTDIR)$(PREFIX)/lib/
	$(INSTALL) -m 755 $(SHARED_LIB) $(DESTDIR)$(PREFIX)/lib/

clean:
	rm -rf $(BUILD_DIR)
