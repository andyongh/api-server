
# api server

High-performance C/Lua API server powered by libmicrohttpd, yyjson, LuaJIT, and jemalloc.

---

## 工程结构

```
jsonrpc_server/
├── Makefile
├── src/
│   ├── log.h            线程安全日志宏（4级别 + 时间戳 + TID）
│   ├── uuid.h/c         RFC-4122 v4 UUID，/dev/urandom
│   ├── auth.h/c         认证桩（valid_* / admin_* token）
│   ├── jsonrpc.h/c      JSONRPC 2.0 协议：解析 + 响应构建（yyjson）
│   ├── task_manager.h/c 异步任务生命周期管理
│   ├── worker_pool.h/c  pthread 线程池
│   ├── methods.h/c      方法注册表 + 内置方法实现
│   ├── server.h/c       MHD 服务器：连接管理 / suspend-resume
│   └── main.c           启动 / 信号 / 优雅关机
├── tools/
│   └── rpc_debug.c      调试 CLI 工具
└── third_party/
    ├── yyjson.h          (make 自动下载)
    └── yyjson.c
```

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

- **64-bucket 哈希表**，每桶独立 mutex，高并发下减少锁竞争
- **Reaper 线程**每 2 秒扫描：超时任务标 `TIMED_OUT`，1小时以上终态任务自动清理
- 任务所有权：只有 owner 或 admin 可取消/查看

---

## 快速上手

```bash
# 构建
make

# optional:
export LD_LIBRARY_PATH=lib:$LD_LIBRARY_PATH     # Linux
export DYLD_LIBRARY_PATH=lib:$DYLD_LIBRARY_PATH # Macos

# 运行（调试日志）
./build/jsonrpc-server -l 0 -p 8080

# 基本调用
./build/rpc_debug ping
./build/rpc_debug -r add '{"a":3,"b":4}'

# 异步任务（自动轮询直到完成）
./build/rpc_debug -r -P slow_compute '{"n":5}'

# 认证测试
./build/rpc_debug -s bad_token ping           # 返回 -32000
./build/rpc_debug -s admin_root task.list     # admin 查看全部

# 任务管理
./build/rpc_debug slow_compute '{"n":10}' | grep task_id   # 拿到ID
./build/rpc_debug task.cancel '{"task_id":"<uuid>"}'
./build/rpc_debug task.refresh '{"task_id":"<uuid>"}'

# 集成冒烟测试
make test

# AddressSanitizer 构建
make asan
```

---

## 扩展 auth_verify

`src/auth.c` 中的 `auth_verify()` 是整个认证的唯一入口，将其换成真实逻辑（JWT 验签、Redis 查询等）而无需改动其他任何文件：

```c
bool auth_verify(const char *token, user_info_t *out) {
    // 例：调用 Redis / JWT 库
    return jwt_verify(token, out->username, out->role);
}
```

## 新增业务方法

在 `src/methods.c` 末尾注册即可：

```c
// 同步
static char *method_my_sync(const user_info_t *u, yyjson_val *p,
                             int *ec, char *em, size_t esz) { ... }

// 注册到表
{ "my.sync",  false, 0,   method_my_sync,  NULL },
{ "my.async", true,  120, NULL, method_my_async_worker },
```