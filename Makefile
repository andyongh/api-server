CC      := gcc
CSTD    := -std=c11
WARN    := -Wall -Wextra -Wshadow -Wstrict-prototypes
OPT     := -O2 -g
DEFINES := -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L

# ── Directories ──────────────────────────────────────────────────────
SRCDIR   := src
TOOLDIR  := tools
DEPSDIR  := deps
BUILDDIR := build

# ── Submodule locations ──────────────────────────────────────────────
YYJSON_DIR       := $(DEPSDIR)/yyjson
LUAJIT_DIR       := $(DEPSDIR)/luajit
JEMALLOC_DIR     := $(DEPSDIR)/jemalloc
MICROHTTPD_DIR   := $(DEPSDIR)/libmicrohttpd

# ── Dependency include flags ─────────────────────────────────────────
DEP_INCLUDES := -I$(YYJSON_DIR)/src \
                -I$(LUAJIT_DIR)/src \
                -I$(JEMALLOC_DIR)/include \
                -I$(MICROHTTPD_DIR)/src/include

# ── Global compiler flags ────────────────────────────────────────────
CFLAGS  := $(CSTD) $(WARN) $(OPT) $(DEFINES) \
            -Isrc -Iinclude $(DEP_INCLUDES)

# ── Static libraries built from submodules ───────────────────────────
LUAJIT_LIB      := $(LUAJIT_DIR)/src/libluajit.a
JEMALLOC_LIB    := $(JEMALLOC_DIR)/lib/libjemalloc.a
MICROHTTPD_LIB  := $(MICROHTTPD_DIR)/src/microhttpd/.libs/libmicrohttpd.a

# ── Linker flags (all libs now static) ───────────────────────────────
LDFLAGS := -lpthread -lm -ldl

# ── Source files ─────────────────────────────────────────────────────
SERVER_SRCS := \
    $(SRCDIR)/main.c         \
    $(SRCDIR)/server.c       \
    $(SRCDIR)/jsonrpc.c      \
    $(SRCDIR)/auth.c         \
    $(SRCDIR)/task_manager.c \
    $(SRCDIR)/worker_pool.c  \
    $(SRCDIR)/methods.c      \
    $(SRCDIR)/uuid.c         \
    $(YYJSON_DIR)/src/yyjson.c

DEBUG_SRCS := $(TOOLDIR)/rpc_debug.c

# ── Object files ─────────────────────────────────────────────────────
SERVER_OBJS := $(patsubst %.c,$(BUILDDIR)/%.o,$(SERVER_SRCS))
DEBUG_OBJS  := $(patsubst %.c,$(BUILDDIR)/%.o,$(DEBUG_SRCS))

# ── Final binaries ───────────────────────────────────────────────────
SERVER_BIN  := $(BUILDDIR)/jsonrpc-server
DEBUG_BIN   := $(BUILDDIR)/rpc_debug

.PHONY: all clean run debug check-deps help init-submodules test asan distclean clean-deps

all: init-submodules check-deps $(SERVER_BIN) $(DEBUG_BIN)
	@echo ""
	@echo "Build complete:"
	@echo "  Server : $(SERVER_BIN)"
	@echo "  Debug  : $(DEBUG_BIN)"

# ── Submodule initialisation ─────────────────────────────────────────
init-submodules:
	@if [ -f .gitmodules ]; then \
		echo "Initializing submodules..."; \
		git submodule update --init --recursive; \
	else \
		echo "No .gitmodules found, skipping submodule initialization."; \
	fi

# ── Dependency checks ────────────────────────────────────────────────
check-deps:
	@if [ ! -d "$(YYJSON_DIR)/src" ]; then \
		echo "ERROR: yyjson submodule missing. Run: make init-submodules"; exit 1; fi
	@if [ ! -d "$(LUAJIT_DIR)/src" ]; then \
		echo "ERROR: luajit submodule missing. Run: make init-submodules"; exit 1; fi
	@if [ ! -d "$(JEMALLOC_DIR)" ]; then \
		echo "ERROR: jemalloc submodule missing. Run: make init-submodules"; exit 1; fi
	@if [ ! -d "$(MICROHTTPD_DIR)/src" ]; then \
		echo "ERROR: libmicrohttpd submodule missing. Run: make init-submodules"; exit 1; fi
	@if [ ! -f "$(MICROHTTPD_LIB)" ]; then \
		echo "WARNING: libmicrohttpd not built yet. It will be built automatically."; fi

# ── Build LuaJIT static library (macOS fix included) ────────────────
$(LUAJIT_LIB): | check-deps
	@echo "Building LuaJIT..."
	@if [ "$$(uname)" = "Darwin" ]; then \
		export MACOSX_DEPLOYMENT_TARGET=$$(sw_vers -productVersion | cut -d. -f1-2) && \
		$(MAKE) -C $(LUAJIT_DIR); \
	else \
		$(MAKE) -C $(LUAJIT_DIR); \
	fi

# ── Build jemalloc static library ────────────────────────────────────
$(JEMALLOC_LIB): | check-deps
	@echo "Building jemalloc..."
	@if [ ! -f $(JEMALLOC_DIR)/configure ]; then \
		echo "Running autogen.sh..."; \
		cd $(JEMALLOC_DIR) && ./autogen.sh; \
	fi
	@if [ ! -f $(JEMALLOC_DIR)/Makefile ]; then \
		cd $(JEMALLOC_DIR) && ./configure --enable-static --disable-shared; \
	fi
	$(MAKE) -C $(JEMALLOC_DIR)

# ── Build libmicrohttpd static library ───────────────────────────────
$(MICROHTTPD_LIB): | check-deps
	@echo "Building libmicrohttpd..."
	@if [ ! -f $(MICROHTTPD_DIR)/configure ]; then \
		echo "Running autoreconf..."; \
		cd $(MICROHTTPD_DIR) && autoreconf -fi; \
	fi
	@if [ ! -f $(MICROHTTPD_DIR)/Makefile ]; then \
		cd $(MICROHTTPD_DIR) && ./configure --enable-static --disable-shared --disable-https --disable-examples --disable-doc; \
	fi
	$(MAKE) -C $(MICROHTTPD_DIR)

# ── Link the server ──────────────────────────────────────────────────
$(SERVER_BIN): $(SERVER_OBJS) $(LUAJIT_LIB) $(JEMALLOC_LIB) $(MICROHTTPD_LIB) | $(BUILDDIR)
	$(CC) $(OPT) -o $@ $(SERVER_OBJS) $(LUAJIT_LIB) $(JEMALLOC_LIB) $(MICROHTTPD_LIB) $(LDFLAGS)
	@echo "Linked: $@"

# ── Link the debug tool ──────────────────────────────────────────────
$(DEBUG_BIN): $(DEBUG_OBJS) | $(BUILDDIR)
	$(CC) $(OPT) -o $@ $^
	@echo "Linked: $@"

# ── Compile rules ────────────────────────────────────────────────────
$(BUILDDIR)/$(SRCDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)/$(SRCDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/$(TOOLDIR)/%.o: $(TOOLDIR)/%.c | $(BUILDDIR)/$(TOOLDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# Special rule for yyjson to suppress unused‑parameter warnings
$(BUILDDIR)/$(DEPSDIR)/yyjson/src/%.o: $(YYJSON_DIR)/src/%.c | $(BUILDDIR)/$(DEPSDIR)/yyjson/src
	$(CC) $(CFLAGS) -Wno-unused-parameter -c -o $@ $<

# ── Directory creation ───────────────────────────────────────────────
$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/$(SRCDIR):
	mkdir -p $(BUILDDIR)/$(SRCDIR)

$(BUILDDIR)/$(TOOLDIR):
	mkdir -p $(BUILDDIR)/$(TOOLDIR)

$(BUILDDIR)/$(DEPSDIR)/yyjson/src:
	mkdir -p $(BUILDDIR)/$(DEPSDIR)/yyjson/src

# ── Dependency tracking (.d files) ──────────────────────────────────
DEPS := $(SERVER_OBJS:.o=.d) $(DEBUG_OBJS:.o=.d)
-include $(DEPS)

$(BUILDDIR)/%.o: CFLAGS += -MMD -MP

# ── Run targets ──────────────────────────────────────────────────────
run: $(SERVER_BIN)
	$(SERVER_BIN) -l 0

run-info: $(SERVER_BIN)
	$(SERVER_BIN) -l 1

# ── Quick smoke test ─────────────────────────────────────────────────
test: $(SERVER_BIN) $(DEBUG_BIN)
	@echo "=== Starting server in background ==="
	@$(SERVER_BIN) -l 1 &
	@SERVER_PID=$$!; \
	sleep 1; \
	echo "=== ping ==="; \
	$(DEBUG_BIN) -r ping; \
	echo "=== add ==="; \
	$(DEBUG_BIN) -r add '{"a":10,"b":32}'; \
	echo "=== echo ==="; \
	$(DEBUG_BIN) -r echo '{"msg":"hello world"}'; \
	echo "=== bad auth ==="; \
	$(DEBUG_BIN) -s bad_token ping; \
	echo "=== async slow_compute (n=2, poll) ==="; \
	$(DEBUG_BIN) -r -P -t 30 slow_compute '{"n":2}'; \
	echo "=== task.list ==="; \
	$(DEBUG_BIN) -r task.list; \
	kill $$SERVER_PID 2>/dev/null; \
	echo "=== done ==="

# ── Sanitiser build ──────────────────────────────────────────────────
asan: CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: LDFLAGS += -fsanitize=address,undefined
asan: OPT     := -O1 -g
asan: all

# ── Clean ────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILDDIR)
	@echo "Cleaned build directory."

clean-deps:
	@if [ -f $(LUAJIT_DIR)/Makefile ]; then $(MAKE) -C $(LUAJIT_DIR) clean; fi
	@if [ -f $(JEMALLOC_DIR)/Makefile ]; then $(MAKE) -C $(JEMALLOC_DIR) clean; fi
	@if [ -f $(MICROHTTPD_DIR)/Makefile ]; then $(MAKE) -C $(MICROHTTPD_DIR) clean; fi
	@echo "Cleaned dependency builds."

distclean: clean clean-deps
	@echo "To remove submodule source, manually delete the 'deps/' directory."

# ── Help ─────────────────────────────────────────────────────────────
help:
	@echo "Targets:"
	@echo "  all            Build server and debug tool (default)"
	@echo "  init-submodules Initialize git submodules (dependencies)"
	@echo "  check-deps     Verify all dependencies are present"
	@echo "  run            Build and run server (debug log level)"
	@echo "  run-info       Build and run server (info log level)"
	@echo "  test           Build, start server, run smoke tests, stop server"
	@echo "  asan           Build with AddressSanitizer + UBSan"
	@echo "  clean          Remove build directory"
	@echo "  clean-deps     Clean built dependency libraries"
	@echo "  distclean      Clean build and dependencies (does not remove submodule source)"
	@echo "  help           Show this help"
	@echo ""
	@echo "Server options:"
	@echo "  -p <port>    Listen port     (default: 8080)"
	@echo "  -t <n>       Worker threads  (default: nCPU/2)"
	@echo "  -q <n>       Queue depth     (default: 256)"
	@echo "  -l <0-3>     Log level       (0=debug, 1=info, 2=warn, 3=error)"