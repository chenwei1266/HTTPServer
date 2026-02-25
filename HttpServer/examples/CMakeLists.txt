# CMakeLists.txt — Todo List 示例
cmake_minimum_required(VERSION 3.10)
project(TodoServer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── 查找 muduo（根据实际安装路径调整）──
# 如果 muduo 装在 /usr/local，通常不需要额外设置。
# 若在自定义路径，取消注释并修改：
# set(MUDUO_INCLUDE_DIR "/path/to/muduo")
# set(MUDUO_LIBRARY_DIR "/path/to/muduo/build")

# ── HttpServer 头文件路径（相对于本文件所在目录）──
set(HTTP_SERVER_ROOT "${CMAKE_SOURCE_DIR}/..")
include_directories(
    ${HTTP_SERVER_ROOT}/include
    ${HTTP_SERVER_ROOT}/include/http
    ${HTTP_SERVER_ROOT}/include/router
    ${HTTP_SERVER_ROOT}/include/session
    ${HTTP_SERVER_ROOT}/include/middleware
    ${HTTP_SERVER_ROOT}/include/ssl
    ${HTTP_SERVER_ROOT}/include/utils
)

# ── HttpServer 源文件 ──
file(GLOB_RECURSE HTTP_SERVER_SRCS
    "${HTTP_SERVER_ROOT}/src/*.cpp"
)

# ── 可执行文件 ──
add_executable(todo_server
    todo_server.cpp
    ${HTTP_SERVER_SRCS}
)

target_link_libraries(todo_server
    muduo_net
    muduo_base
    pthread
    ssl
    crypto
    mysqlcppconn      # MySQL Connector/C++（DbConnection.cpp 需要）
)

# 构建后自动把 todo.html 复制到可执行文件旁边
add_custom_command(TARGET todo_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/todo.html
        $<TARGET_FILE_DIR:todo_server>/todo.html
    COMMENT "复制 todo.html 到输出目录"
)