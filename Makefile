# Focus Cube for ESP-Mosaico
#
#   make test   - host unit tests for the core logic (gcc only)
#   make web    - browser simulator into build/web (needs emcc on PATH)
#   make serve  - serve build/web on http://localhost:8000
#   make deps   - fetch LVGL into third_party/ (run on its own, before 'make web')

LVGL_VERSION := v9.2.2
LVGL_DIR     := third_party/lvgl

CFLAGS_COMMON := -std=c99 -Wall -Wextra

# ---------------------------------------------------------------- host tests
HOST_CC := gcc $(CFLAGS_COMMON) -Werror -g -fsanitize=address,undefined -Icore

.PHONY: test
test: build/host/test_focus_core build/host/test_sensors
	./build/host/test_focus_core
	./build/host/test_sensors

build/host/test_focus_core: core/focus_core.c core/focus_core.h tests/test_focus_core.c
	@mkdir -p $(dir $@)
	$(HOST_CC) core/focus_core.c tests/test_focus_core.c -o $@

build/host/test_sensors: core/sensors.c core/sensors.h tests/test_sensors.c
	@mkdir -p $(dir $@)
	$(HOST_CC) core/sensors.c tests/test_sensors.c -lm -o $@

# ---------------------------------------------------------------- web build
EMCC      ?= emcc
WEB_OUT   := build/web
WEB_OBJ   := build/web-obj
WEB_INC   := -Icore -Iapp -Iplatform/web -I$(LVGL_DIR) -DLV_CONF_INCLUDE_SIMPLE
WEB_CFLAGS := -O2 $(WEB_INC)

LVGL_SRCS := $(shell find $(LVGL_DIR)/src -name '*.c' 2>/dev/null)
APP_SRCS  := core/focus_core.c core/sensors.c app/focus_app.c app/focus_ui.c app/lab_app.c \
             platform/web/main.c

LVGL_OBJS := $(patsubst %.c,$(WEB_OBJ)/%.o,$(LVGL_SRCS))
APP_OBJS  := $(patsubst %.c,$(WEB_OBJ)/%.o,$(APP_SRCS))

EXPORT_LIST := _web_init _web_frame _web_framebuffer _web_set_accel _web_pointer _web_set_time_scale \
               _web_set_orientation _web_set_spin _web_shake _web_set_magnet _web_set_noise _web_clap \
               _web_set_battery _web_set_fn _web_get_heading _web_readout _malloc
comma := ,
EXPORTS := $(subst $(eval) ,$(comma),$(strip $(EXPORT_LIST)))

.PHONY: web
WEB_STATIC := index.html lab.html common.js sim.js lab.js sim.css
web: $(WEB_OUT)/focuscube.js $(addprefix $(WEB_OUT)/,$(WEB_STATIC))

$(WEB_OUT)/focuscube.js: $(LVGL_OBJS) $(APP_OBJS)
	@test -n "$(LVGL_SRCS)" || { echo "LVGL not found in $(LVGL_DIR): run 'make deps' first, as a separate command"; exit 1; }
	@mkdir -p $(dir $@)
	$(EMCC) $^ -O2 -o $@ \
	    -sEXPORTED_FUNCTIONS=$(EXPORTS) \
	    -sEXPORTED_RUNTIME_METHODS=HEAPU8 \
	    -sMODULARIZE=1 -sEXPORT_NAME=createFocusCube \
	    -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=web

$(WEB_OUT)/%: platform/web/%
	@mkdir -p $(dir $@)
	cp $< $@

# LVGL objects don't depend on app headers; app objects get -Werror.
$(WEB_OBJ)/$(LVGL_DIR)/%.o: $(LVGL_DIR)/%.c platform/web/lv_conf.h
	@mkdir -p $(dir $@)
	@$(EMCC) $(WEB_CFLAGS) -c $< -o $@

$(WEB_OBJ)/%.o: %.c $(wildcard core/*.h app/*.h) platform/web/lv_conf.h
	@mkdir -p $(dir $@)
	$(EMCC) $(WEB_CFLAGS) -std=gnu11 -Wall -Wextra -Werror -c $< -o $@

.PHONY: serve
serve: web
	cd $(WEB_OUT) && python3 -m http.server 8000

# ---------------------------------------------------------------- misc
.PHONY: deps
deps:
	@test -d $(LVGL_DIR) || git clone --depth 1 --branch $(LVGL_VERSION) \
	    https://github.com/lvgl/lvgl.git $(LVGL_DIR)

.PHONY: clean
clean:
	rm -rf build
