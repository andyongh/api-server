# ═══════════════════════════════════════════════════════════════════
#  JSONRPC 2.0 Server — Makefile
# ═══════════════════════════════════════════════════════════════════

CC      := gcc
CSTD    := -std=c11
WARN    := -Wall -Wextra -Wshadow -Wstrict-prototypes
OPT     := -O2 -g
DEFINES := -D_GNU_SOURCE -D_POSIX_C_SOURCE=200809L

CFLAGS  := $(CSTD) $(WARN) $(OPT) $(DEFINES) \
            -Isrc -Ithird_party -Iinclude

LDFLAGS := -Llib -lmicrohttpd -lpthread -lm


# ── Directories ──────────────────────────────────────────────────────
SRCDIR   := src
TOOLDIR  := tools
TPDIR    := third_party
BUILDDIR := build

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
    $(TPDIR)/yyjson.c

DEBUG_SRCS := $(TOOLDIR)/rpc_debug.c

# ── Object files ─────────────────────────────────────────────────────
SERVER_OBJS := $(patsubst %.c,$(BUILDDIR)/%.o,$(SERVER_SRCS))
DEBUG_OBJS  := $(patsubst %.c,$(BUILDDIR)/%.o,$(DEBUG_SRCS))

# ── Targets ──────────────────────────────────────────────────────────
SERVER_BIN  := $(BUILDDIR)/jsonrpc-server
DEBUG_BIN   := $(BUILDDIR)/rpc_debug

.PHONY: all clean run debug check-deps help

all: check-deps $(SERVER_BIN) $(DEBUG_BIN)
	@echo ""
	@echo "Build complete:"
	@echo "  Server : $(SERVER_BIN)"
	@echo "  Debug  : $(DEBUG_BIN)"

# ── Dependency check ─────────────────────────────────────────────────
check-deps:
# 	@if [ ! -f /usr/include/microhttpd.h ] && [ ! -f /usr/local/include/microhttpd.h ]; then \
# 		echo "ERROR: libmicrohttpd-dev not found."; \
# 		echo "  Ubuntu/Debian: sudo apt-get install libmicrohttpd-dev"; \
# 		echo "  Fedora/RHEL:   sudo dnf install libmicrohttpd-devel"; \
# 		exit 1; \
# 	fi
	@if [ ! -f $(TPDIR)/yyjson.h ] || [ ! -f $(TPDIR)/yyjson.c ]; then \
		echo "yyjson not found — downloading..."; \
		$(MAKE) $(TPDIR)/yyjson.h; \
	fi

# ── Download yyjson ───────────────────────────────────────────────────
$(TPDIR)/yyjson.h $(TPDIR)/yyjson.c:
	@mkdir -p $(TPDIR)
	@echo "Downloading yyjson 0.10.0..."
	@curl -sL https://github.com/ibireme/yyjson/archive/refs/tags/0.10.0.tar.gz \
	    | tar -xz --strip-components=2 -C $(TPDIR) \
	      yyjson-0.10.0/src/yyjson.h \
	      yyjson-0.10.0/src/yyjson.c
	@echo "yyjson downloaded."

# ── Server binary ─────────────────────────────────────────────────────
$(SERVER_BIN): $(SERVER_OBJS) | $(BUILDDIR)
	$(CC) $(OPT) -o $@ $^ $(LDFLAGS)
	@echo "Linked: $@"

# ── Debug tool ────────────────────────────────────────────────────────
$(DEBUG_BIN): $(DEBUG_OBJS) | $(BUILDDIR)
	$(CC) $(OPT) -o $@ $^
	@echo "Linked: $@"

# ── Compile rules ─────────────────────────────────────────────────────
$(BUILDDIR)/$(SRCDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)/$(SRCDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/$(TOOLDIR)/%.o: $(TOOLDIR)/%.c | $(BUILDDIR)/$(TOOLDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/$(TPDIR)/%.o: $(TPDIR)/%.c | $(BUILDDIR)/$(TPDIR)
	$(CC) $(CFLAGS) -Wno-unused-parameter -c -o $@ $<

# ── Directory creation ────────────────────────────────────────────────
$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/$(SRCDIR):
	mkdir -p $(BUILDDIR)/$(SRCDIR)

$(BUILDDIR)/$(TOOLDIR):
	mkdir -p $(BUILDDIR)/$(TOOLDIR)

$(BUILDDIR)/$(TPDIR):
	mkdir -p $(BUILDDIR)/$(TPDIR)

# ── Dependency tracking (.d files) ───────────────────────────────────
DEPS := $(SERVER_OBJS:.o=.d) $(DEBUG_OBJS:.o=.d)
-include $(DEPS)

$(BUILDDIR)/%.o: CFLAGS += -MMD -MP

# ── Run ───────────────────────────────────────────────────────────────
run: $(SERVER_BIN)
# 	export LD_LIBRARY_PATH=lib:$LD_LIBRARY_PATH     # Linux
# 	export DYLD_LIBRARY_PATH=lib:$DYLD_LIBRARY_PATH # Macos
	$(SERVER_BIN) -l 0

run-info: $(SERVER_BIN)
	$(SERVER_BIN) -l 1

# ── Quick smoke test ──────────────────────────────────────────────────
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

# ── Address sanitiser build ───────────────────────────────────────────
asan: CFLAGS  += -fsanitize=address,undefined -fno-omit-frame-pointer
asan: LDFLAGS += -fsanitize=address,undefined
asan: OPT     := -O1 -g
asan: all

# ── Clean ─────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILDDIR)

distclean: clean
	rm -f $(TPDIR)/yyjson.h $(TPDIR)/yyjson.c

# ── Help ──────────────────────────────────────────────────────────────
help:
	@echo "Targets:"
	@echo "  all          Build server and debug tool (default)"
	@echo "  run          Build and run server (debug log level)"
	@echo "  run-info     Build and run server (info log level)"
	@echo "  test         Build, start server, run smoke tests, stop server"
	@echo "  asan         Build with AddressSanitizer + UBSan"
	@echo "  clean        Remove build directory"
	@echo "  distclean    Remove build directory and downloaded yyjson"
	@echo "  help         Show this help"
	@echo ""
	@echo "Server options:"
	@echo "  -p <port>    Listen port     (default: 8080)"
	@echo "  -t <n>       Worker threads  (default: nCPU/2)"
	@echo "  -q <n>       Queue depth     (default: 256)"
	@echo "  -l <0-3>     Log level       (0=debug, 1=info, 2=warn, 3=error)"
