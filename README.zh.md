# api server

High-performance C/Lua API server powered by libmicrohttpd, yyjson, LuaJIT, and jemalloc.

---

## 工程结构

```
.
├── Makefile
├── deps/                    # 所有第三方依赖（Git 子模块）
│   ├── yyjson/              #   yyjson  (JSON 解析)
│   ├── luajit/              #   LuaJIT (Lua 引擎)
│   ├── jemalloc/            #   jemalloc (内存分配器)
│   └── libmicrohttpd/       #   libmicrohttpd (HTTP 服务器)
├── src/
│   ├── main.c               # 入口：启动、信号、优雅关机
│   ├── server.c / server.h  # MHD 服务器：连接管理、suspend/resume
│   ├── jsonrpc.c / jsonrpc.h# JSON‑RPC 2.0 协议解析与响应构建（yyjson）
│   ├── auth.c / auth.h      # 认证桩（valid_* / admin_* 令牌）
│   ├── task_manager.c / .h  # 异步任务生命周期管理（64桶哈希表）
│   ├── worker_pool.c / .h   # 线程池（pthread）
│   ├── methods.c / methods.h# 方法注册表及内置业务实现
│   ├── uuid.c / uuid.h      # RFC‑4122 v4 UUID（/dev/urandom）
│   └── log.h                # 线程安全日志宏（4级别 + 时间戳 + TID）
├── tools/
│   └── rpc_debug.c          # 调试 CLI 工具
├── LICENSE
└── README.md
```

> **注意**：所有 `deps/` 下的第三方库均以 Git 子模块（submodule）形式管理。
> 首次构建时会自动初始化和编译这些依赖，无需手动下载或安装。

---

## 关键设计

### MHD 并发控制

| 参数                              | 值                              |
| --------------------------------- | ------------------------------- |
| `MHD_USE_INTERNAL_POLLING_THREAD` | MHD 内部线程模式                |
| `MHD_ALLOW_SUSPEND_RESUME`        | 连接可挂起/恢复（异步派发核心） |
| `MHD_OPTION_THREAD_POOL_SIZE`     | `max(1, nCPU/2)`                |
| `MHD_OPTION_CONNECTION_LIMIT`     | 3（最大并发连接数）             |

### 同步方法（suspend/resume 流程）

```
MHD handler → 解析 → 认证 → MHD_suspend_connection()
    → worker_pool_submit() → 返回 MHD_YES
                                   ↓ worker线程
                            执行业务 → 写入 ci->resp_body
                            MHD_resume_connection()
                                   ↓ MHD唤醒
MHD handler 再次被调用 → state=CI_READY → 发送响应
```

### 异步方法（202 ACCEPTED 流程）

```
MHD handler → task_create() → worker_pool_submit(async_work)
           → 立刻返回 202 ACCEPTED + task_id
                                   ↓ 后台worker
                            task_set_running()
                            真正执行（可分钟级）
                            task_complete() / task_fail()
客户端 → task.status  → 轮询结果
       → task.cancel  → 取消
       → task.refresh → 重置超时 deadline
       → task.list    → 列出本用户所有任务
```

### Task Manager

- **64‑bucket 哈希表**，每桶独立 mutex，高并发下减少锁竞争
- **Reaper 线程**每 2 秒扫描：超时任务标 `TIMED_OUT`，1小时以上终态任务自动清理
- 任务所有权：只有 owner 或 admin 可取消/查看

---

## 快速上手

### 构建

```bash
# 克隆项目（包含子模块）
git clone --recurse-submodules <你的仓库地址>
# 如果已克隆，手动初始化子模块
git submodule update --init --recursive

# 构建（自动编译所有依赖和项目）
make
```

编译完成后，所有依赖库将**静态链接**进最终二进制文件，无需额外设置库路径。

### 运行与调试

```bash
# 启动服务器（调试日志）
./build/jsonrpc-server -l 0 -p 8080

# 基本 JSON‑RPC 调用
./build/rpc_debug ping
./build/rpc_debug -r add '{"a":3,"b":4}'

# 异步任务（自动轮询直到完成）
./build/rpc_debug -r -P slow_compute '{"n":5}'

# 认证测试
./build/rpc_debug -s bad_token ping           # 返回 -32000
./build/rpc_debug -s admin_root task.list     # admin 查看全部任务

# 任务管理
./build/rpc_debug slow_compute '{"n":10}' | grep task_id   # 获取 task_id
./build/rpc_debug task.cancel '{"task_id":"<uuid>"}'
./build/rpc_debug task.refresh '{"task_id":"<uuid>"}'

# 集成冒烟测试
make test

# 使用 AddressSanitizer 构建
make asan
```

---

## 扩展指南

### 认证模块

`src/auth.c` 中的 `auth_verify()` 是整个认证的唯一入口，将其替换为真实逻辑（JWT 验签、Redis 查询等）而无需改动其他任何文件：

```c
bool auth_verify(const char *token, user_info_t *out) {
    // 例：调用 Redis / JWT 库
    return jwt_verify(token, out->username, out->role);
}
```

### 新增业务方法

在 `src/methods.c` 末尾注册即可：

```c
// 同步方法
static char *method_my_sync(const user_info_t *u, yyjson_val *p,
                             int *ec, char *em, size_t esz) {
    // 业务逻辑
    return strdup("...");
}

// 异步方法（worker 函数）
static void method_my_async_worker(task_t *task) {
    // 长时间运算
    task_complete(task, "result");
}

// 注册到方法表
static const method_entry_t methods[] = {
    ...
    { "my.sync",  false, 0,   method_my_sync,  NULL },
    { "my.async", true,  120, NULL, method_my_async_worker },
};
