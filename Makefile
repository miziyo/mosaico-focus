# Focus Cube for ESP-Mosaico
#
#   make test   - host unit tests for the core logic (gcc only)
#   make web    - browser simulator into build/web (needs emcc on PATH)
#   make serve  - serve build/web on http://localhost:8000
#   make deps   - fetch LVGL into third_party/

LVGL_VERSION := v9.2.2
LVGL_DIR     := third_party/lvgl

CFLAGS_COMMON := -std=c99 -Wall -Wextra

# ---------------------------------------------------------------- host tests
TEST_BIN := build/host/test_focus_core

.PHONY: test
test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): core/focus_core.c core/focus_core.h tests/test_focus_core.c
	@mkdir -p $(dir $@)
	gcc $(CFLAGS_COMMON) -Werror -g -fsanitize=address,undefined -Icore \
	    core/focus_core.c tests/test_focus_core.c -o $@

# ---------------------------------------------------------------- web build
EMCC      ?= emcc
WEB_OUT   := build/web
WEB_OBJ   := build/web-obj
WEB_INC   := -Icore -Iapp -Iplatform/web -I$(LVGL_DIR) -DLV_CONF_INCLUDE_SIMPLE
WEB_CFLAGS := -O2 $(WEB_INC)

LVGL_SRCS := $(shell find $(LVGL_DIR)/src -name '*.c' 2>/dev/null)
APP_SRCS  := core/focus_core.c app/focus_app.c app/focus_ui.c platform/web/main.c

LVGL_OBJS := $(patsubst %.c,$(WEB_OBJ)/%.o,$(LVGL_SRCS))
APP_OBJS  := $(patsubst %.c,$(WEB_OBJ)/%.o,$(APP_SRCS))

EXPORTS := _web_init,_web_frame,_web_framebuffer,_web_set_accel,_web_pointer,_web_set_time_scale,_malloc

.PHONY: web
web: $(WEB_OUT)/focuscube.js $(WEB_OUT)/index.html $(WEB_OUT)/sim.js $(WEB_OUT)/sim.css

$(WEB_OUT)/focuscube.js: $(LVGL_OBJS) $(APP_OBJS)
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
