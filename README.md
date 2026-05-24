# api server

High-performance C/Lua API server powered by libmicrohttpd, yyjson, LuaJIT, and jemalloc.

---

## Project Structure

```
.
├── Makefile
├── deps/                    # All third-party dependencies (Git submodules)
│   ├── yyjson/              #   yyjson  (JSON parser)
│   ├── luajit/              #   LuaJIT (Lua engine)
│   ├── jemalloc/            #   jemalloc (memory allocator)
│   └── libmicrohttpd/       #   libmicrohttpd (HTTP server)
├── src/
│   ├── main.c               # Entry point: startup, signals, graceful shutdown
│   ├── server.c / server.h  # MHD server: connection management, suspend/resume
│   ├── jsonrpc.c / jsonrpc.h# JSON‑RPC 2.0 parsing & response construction (yyjson)
│   ├── auth.c / auth.h      # Authentication stubs (valid_* / admin_* tokens)
│   ├── task_manager.c / .h  # Async task lifecycle manager (64‑bucket hash table)
│   ├── worker_pool.c / .h   # Thread pool (pthread)
│   ├── methods.c / methods.h# Method registry & built-in implementations
│   ├── uuid.c / uuid.h      # RFC‑4122 v4 UUID (/dev/urandom)
│   └── log.h                # Thread-safe logging macros (4 levels + timestamp + TID)
├── tools/
│   └── rpc_debug.c          # Debug CLI tool
├── LICENSE
└── README.md
```

> **Note**: All libraries under `deps/` are managed as Git submodules.
> The first build will automatically initialize and compile these dependencies – no manual downloads or installations required.

---

## Key Design

### MHD Concurrency Control

| Parameter                         | Value                                |
| --------------------------------- | ------------------------------------ |
| `MHD_USE_INTERNAL_POLLING_THREAD` | MHD internal thread mode             |
| `MHD_ALLOW_SUSPEND_RESUME`        | Suspend/resume for async dispatching |
| `MHD_OPTION_THREAD_POOL_SIZE`     | `max(1, nCPU/2)`                     |
| `MHD_OPTION_CONNECTION_LIMIT`     | 3 (max concurrent connections)       |

### Synchronous Methods (suspend/resume flow)

```
MHD handler → parse → auth → MHD_suspend_connection()
    → worker_pool_submit() → return MHD_YES
                                   ↓ worker thread
                            execute business logic → write ci->resp_body
                            MHD_resume_connection()
                                   ↓ MHD wakes up
MHD handler invoked again → state=CI_READY → send response
```

### Asynchronous Methods (202 ACCEPTED flow)

```
MHD handler → task_create() → worker_pool_submit(async_work)
           → immediately return 202 ACCEPTED + task_id
                                   ↓ background worker
                            task_set_running()
                            real work (can take minutes)
                            task_complete() / task_fail()
Client → task.status  → poll for result
       → task.cancel  → cancel
       → task.refresh → reset timeout deadline
       → task.list    → list all tasks for this user
```

### Task Manager

- **64‑bucket hash table**, each bucket with its own mutex – reduces lock contention under high concurrency.
- **Reaper thread** scans every 2 seconds: expired tasks marked `TIMED_OUT`, terminal tasks older than 1 hour automatically cleaned up.
- Task ownership: only the owner or an admin can cancel / view a task.

---

## Quick Start

### Build

```bash
# Clone with submodules
git clone --recurse-submodules <your-repo-url>
# If already cloned, init submodules manually
git submodule update --init --recursive

# Build (automatically compiles all deps and the project)
make
```

All dependencies are statically linked into the final binaries – no extra library path configuration needed.

### Run & Test

```bash
# Start the server (debug logging)
./build/jsonrpc-server -l 0 -p 8080

# Basic JSON‑RPC calls
./build/rpc_debug ping
./build/rpc_debug -r add '{"a":3,"b":4}'

# Async task (auto-polls until complete)
./build/rpc_debug -r -P slow_compute '{"n":5}'

# Auth tests
./build/rpc_debug -s bad_token ping           # returns -32000
./build/rpc_debug -s admin_root task.list     # admin sees all tasks

# Task management
./build/rpc_debug slow_compute '{"n":10}' | grep task_id   # get task_id
./build/rpc_debug task.cancel '{"task_id":"<uuid>"}'
./build/rpc_debug task.refresh '{"task_id":"<uuid>"}'

# Smoke tests
make test

# Build with AddressSanitizer
make asan
```

---

## Extension Guide

### Authentication

`auth_verify()` in `src/auth.c` is the single entry point for all auth. Replace it with real logic (JWT validation, Redis lookup, etc.) without touching any other file:

```c
bool auth_verify(const char *token, user_info_t *out) {
    // e.g., call JWT / Redis
    return jwt_verify(token, out->username, out->role);
}
```

### Adding Business Methods

Register new methods at the end of `src/methods.c`:

```c
// Synchronous handler
static char *method_my_sync(const user_info_t *u, yyjson_val *p,
                             int *ec, char *em, size_t esz) {
    // business logic
    return strdup("...");
}

// Asynchronous worker
static void method_my_async_worker(task_t *task) {
    // long-running computation
    task_complete(task, "result");
}

// Register in the method table
static const method_entry_t methods[] = {
    ...
    { "my.sync",  false, 0,   method_my_sync,  NULL },
    { "my.async", true,  120, NULL, method_my_async_worker },
};
```

## Submodule management

- add Submodule

```sh
git submodule add https://github.com/ibireme/yyjson deps/yyjson
git submodule add https://github.com/luajit/luajit deps/luajit
git submodule add https://github.com/jemalloc/jemalloc deps/jemalloc
git submodule add https://git.gnunet.org/libmicrohttpd.git deps/libmicrohttpd
```

- remove Submodule

```sh
git submodule deinit -f deps/libmicrohttpd
git rm -f deps/libmicrohttpd
rm -rf .git/modules/deps/libmicrohttpd
```
