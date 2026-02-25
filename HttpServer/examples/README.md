# Todo List 示例 — 运行指南

基于 HttpServer 框架实现的 Todo List 应用，包含前端页面和后端 REST 接口。

---

## 文件说明

```
todo_server.cpp   后端主程序（路由 + 处理器）
todo.html         前端页面（原生 HTML/CSS/JS，无需打包）
CMakeLists.txt    CMake 构建脚本
```

---

## 一、放置文件

将这三个文件放到项目的 `examples/` 目录下：

```
HttpServer/
├── examples/
│   ├── todo_server.cpp   ← 放这里
│   ├── todo.html         ← 放这里
│   └── CMakeLists.txt    ← 放这里（可选，也可以并入项目顶层 CMake）
├── include/
└── src/
```

---

## 二、方式 A — 使用 CMake 构建（推荐）

### 1. 进入 examples 目录

```bash
cd ~/cw/HTTPServer/HttpServer/examples
```

### 2. 创建并进入构建目录

```bash
mkdir build && cd build
```

### 3. 运行 CMake 配置

```bash
cmake ..
```

> 若 muduo 安装在非标准路径，需要额外指定：
> ```bash
> cmake .. \
>   -DMUDUO_INCLUDE_DIR=/path/to/muduo \
>   -DMUDUO_LIBRARY_DIR=/path/to/muduo/build/lib
> ```

### 4. 编译

```bash
make -j$(nproc)
```

### 5. 启动服务器

```bash
./todo_server
# 或者手动指定 todo.html 路径：
./todo_server ../todo.html
```

看到以下输出说明启动成功：

```
========================================
  Todo Server 已启动
  浏览器访问: http://localhost:8080/
========================================
```

---

## 三、方式 B — 手动 g++ 编译（快速验证）

在项目根目录（HttpServer/）下执行：

```bash
# 收集所有框架源文件
SRC_FILES=$(find src/ -name "*.cpp")

# 编译（注意 -I 路径）
g++ -std=c++17 -O2 \
    -I include \
    examples/todo_server.cpp \
    $SRC_FILES \
    -lmuduo_net -lmuduo_base -lpthread -lssl -lcrypto \
    -o todo_server

# 将 html 复制到当前目录并启动
cp examples/todo.html ./todo.html
./todo_server
```

---

## 四、打开页面

服务器启动后，在浏览器访问：

```
http://localhost:8080/
```

---

## 五、接口说明（可用 curl 测试）

### 查看所有 todo
```bash
curl http://localhost:8080/todos
# 返回：["阅读 HttpServer 框架源码","完成今天的任务清单"]
```

### 添加一条 todo
```bash
curl -X POST http://localhost:8080/todos \
     -H "Content-Type: application/json" \
     -d '{"text":"学习路由注册"}'
# 返回：{"status":"created","text":"学习路由注册"}
```

---

## 六、常见问题

| 现象 | 原因 | 解决 |
|------|------|------|
| `libmuduo_net.so not found` | muduo 未安装或路径未加入 ld | `sudo ldconfig` 或 `export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH` |
| 页面显示"找不到 todo.html" | html 和可执行文件不在同一目录 | 用 `./todo_server /绝对路径/todo.html` 指定 |
| 浏览器跨域报错 | 前端和后端端口不同 | 后端已在响应头加 `Access-Control-Allow-Origin: *`，刷新页面即可 |
| 端口 8080 已占用 | 其他程序占用 | `lsof -i:8080` 找到 PID 后 kill，或修改 `main()` 中的端口号 |
