# Display MOD settings app.
#   make            -> build/arm64/displaymod-ui (static, for the CardputerZero)
#   make test       -> build/host/displaymod-ui-test (headless screenshots)
LVGL_SRC := $(shell find lvgl/src -name '*.c')
DM_VERSION := $(shell sed -n 's/^Version: //p' ../../pkg/DEBIAN/control)
CFLAGS_COMMON := -O2 -I. -Ilvgl -DLV_CONF_INCLUDE_SIMPLE -Wall -Wno-unused-parameter -DDM_VERSION=\"$(DM_VERSION)\"

ARM_CC  := aarch64-linux-gnu-gcc
HOST_CC := gcc

ARM_OBJ  := $(patsubst %.c,build/arm64/%.o,$(LVGL_SRC))
HOST_OBJ := $(patsubst %.c,build/host/%.o,$(LVGL_SRC))

all: build/arm64/displaymod-ui
test: build/host/displaymod-ui-test

build/arm64/%.o: %.c lv_conf.h ../../pkg/DEBIAN/control
	@mkdir -p $(dir $@)
	@$(ARM_CC) $(CFLAGS_COMMON) -DLV_USE_LINUX_FBDEV=1 -c $< -o $@

build/host/%.o: %.c lv_conf.h ../../pkg/DEBIAN/control
	@mkdir -p $(dir $@)
	@$(HOST_CC) $(CFLAGS_COMMON) -DLV_USE_LINUX_FBDEV=0 -c $< -o $@

build/arm64/displaymod-ui: $(ARM_OBJ) build/arm64/ui.o build/arm64/backend_ctl.o build/arm64/main_dev.o
	$(ARM_CC) -static -s -o $@ $^ -lm

build/host/displaymod-ui-test: $(HOST_OBJ) build/host/ui.o build/host/backend_fake.o build/host/main_test.o
	$(HOST_CC) -o $@ $^ -lm

clean:
	rm -rf build
.PHONY: all test clean
