# HTTP Server Benchmark Tools

三个独立的压测工具 + 一个统一 runner，用于测试 HTTP 服务器的不同方面。

## 编译

```bash
cd bench
mkdir build && cd build
cmake ..
make -j
```

## 工具说明

### 1. bench_login - 登录接口 QPS 测试

测试 `POST /api/auth/login` 的 QPS 上限和延迟分布。

**用法：**
```bash
./bench_login <host> <port> <threads> <requests_per_thread> <username> <password> [--csv-out <path>]
```

**示例：**
```bash
./bench_login 127.0.0.1 8080 10 1000 testuser testpass
```

**特性：**
- 多线程并发请求
- HTTP Keep-Alive 连接复用
- 统计 P50/P95/P99 延迟和 QPS
- 区分网络失败与 HTTP 非 200 失败
- TCP_NODELAY 优化

---

### 2. bench_sse - SSE 并发连接测试

测试 `POST /api/chat/stream` 的并发连接数上限和 TTFT（首 token 延迟）。

**用法：**
```bash
./bench_sse <host> <port> <num_connections> \
  [--stages 100,300,500] \
  [--duration 60] \
  [--model <model>] \
  [--enable-tools] \
  [--csv-out <path>] \
  [--json-out <path>]
```

**示例：**
```bash
./bench_sse 127.0.0.1 8080 100 --stages 100,300,500 --duration 30 \
  --csv-out sse.csv --json-out sse.json
```

**特性：**
- 使用 epoll + 非阻塞 I/O 维持 N 条连接
- 统计 TTFT（Time To First Token）
- 统计连接成功率和中途断开次数
- 统计 token 事件吞吐（token events / sec）
- 支持分阶段压测与 CSV/JSON 导出

---

### 3. bench_db - 数据库连接池压测

测试 `GET /api/conversations` 在不同并发下的表现，评估数据库连接池。

**用法：**
```bash
./bench_db <host> <port> <username> <password> \
  [--duration 30] \
  [--stages 10,50,100,200] \
  [--csv-out <path>]
```

**示例：**
```bash
./bench_db 127.0.0.1 8080 testuser testpass --duration 20 \
  --stages 20,100,300 --csv-out db.csv > db_stdout.csv
```

**特性：**
- 自动登录获取 Session Cookie，并在每个 stage 使用有效 session
- 梯度加压：支持自定义 stage
- 每档时长可配置
- 输出 CSV 格式（concurrency, qps, p95/p99 latency, error rate）
- 方便用 Excel/Python 画图分析

---

### 4. run_bench.sh - 一键基线压测

从编译到执行一次跑完三类压测，并按时间戳输出结果目录。

**用法：**
```bash
./run_bench.sh <host> <port> <username> <password> [result_dir]
```

**示例：**
```bash
./run_bench.sh 127.0.0.1 8080 testuser testpass
```

默认输出到 `results/<UTC timestamp>/`，包含：
- `login.csv` / `login.txt`
- `db.csv` / `db_stdout.csv` / `db_stderr.log`
- `sse.csv` / `sse.json` / `sse.txt`

---

## 依赖

仅依赖系统库：
- pthread（多线程）
- 标准 socket API（网络通信）
- epoll（Linux 事件驱动 I/O）

无需 libcurl 或其他第三方库。

## 注意事项

1. 确保服务器已启动并监听指定端口
2. 测试前需要创建测试用户（username/password）
3. `bench_db` 会在 stdout 输出 CSV，也可通过 `--csv-out` 写文件
4. 高并发测试时注意系统 `ulimit -n`（文件描述符数量）
5. `bench_sse` 的 token 吞吐为事件级指标（用于横向比较），非精确 tokenizer 计数
