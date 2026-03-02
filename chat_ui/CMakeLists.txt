cmake_minimum_required(VERSION 3.10)
project(MyMuduoTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =========================================
# 1. 设置 my_muduo 根目录
# =========================================
set(MY_MUDUO_ROOT "${CMAKE_SOURCE_DIR}/..")

# =========================================
# 2. 头文件目录
# =========================================
include_directories(
    ${MY_MUDUO_ROOT}
    ${MY_MUDUO_ROOT}/include
)

# =========================================
# 3. 递归收集 my_muduo 源码
# （如果你还没做成静态库）
# =========================================
file(GLOB_RECURSE MY_MUDUO_SRCS
    "${MY_MUDUO_ROOT}/src/*.cc"
    "${MY_MUDUO_ROOT}/src/*.cpp"
    "${MY_MUDUO_ROOT}/*.cc"
)

# =========================================
# 4. test_new_connection（服务器）
# =========================================
add_executable(test_new_connection
    test_new_connection.cc
    ${MY_MUDUO_SRCS}
)

target_link_libraries(test_new_connection
    muduo_net
    muduo_base
    pthread
)

# =========================================
# 5. test_client（纯socket客户端）
# =========================================
add_executable(test_client
    test_client.cc
)

target_link_libraries(test_client
    pthread
)