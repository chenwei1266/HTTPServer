# HTTP Server Benchmark Tools

三个独立的压测工具，用于测试 HTTP 服务器的不同方面。

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
./bench_login <host> <port> <threads> <requests_per_thread> <username> <password>
```

**示例：**
```bash
./bench_login 127.0.0.1 8080 10 1000 testuser testpass
```

**特性：**
- 多线程并发请求
- HTTP Keep-Alive 连接复用
- 统计 P50/P95/P99 延迟和 QPS
- TCP_NODELAY 优化

---

### 2. bench_sse - SSE 并发连接测试

测试 `POST /api/chat/stream` 的并发连接数上限和 TTFT（首 token 延迟）。

**用法：**
```bash
./bench_sse <host> <port> <num_connections>
```

**示例：**
```bash
./bench_sse 127.0.0.1 8080 100
```

**特性：**
- 使用 epoll 维持 N 条长连接
- 统计 TTFT（Time To First Token）
- 统计连接成功率和中途断开次数
- 非阻塞 I/O

---

### 3. bench_db - 数据库连接池压测

测试 `GET /api/conversations` 在不同并发下的表现，评估数据库连接池。

**用法：**
```bash
./bench_db <host> <port> <username> <password>
```

**示例：**
```bash
./bench_db 127.0.0.1 8080 testuser testpass > results.csv
```

**特性：**
- 自动登录获取 Session Cookie
- 梯度加压：10 → 50 → 100 → 200 并发
- 每档运行 30 秒
- 输出 CSV 格式（concurrency, qps, p99_latency_us, error_rate_percent）
- 方便用 Excel/Python 画图分析

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
3. bench_db 会输出 CSV 到 stdout，stderr 输出日志
4. 高并发测试时注意系统 ulimit 设置（文件描述符数量）
