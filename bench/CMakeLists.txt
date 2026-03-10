cmake_minimum_required(VERSION 3.10)
project(HTTPServerBench)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler flags for optimization
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Wall -Wextra")

# Find threads library
find_package(Threads REQUIRED)

# bench_login - Login QPS benchmark
add_executable(bench_login bench_login.cpp)
target_link_libraries(bench_login PRIVATE Threads::Threads)

# bench_sse - SSE concurrent connections benchmark
add_executable(bench_sse bench_sse.cpp)
target_link_libraries(bench_sse PRIVATE Threads::Threads)

# bench_db - Database connection pool benchmark
add_executable(bench_db bench_db.cpp)
target_link_libraries(bench_db PRIVATE Threads::Threads)

# Installation (optional)
install(TARGETS bench_login bench_sse bench_db
        RUNTIME DESTINATION bin)
