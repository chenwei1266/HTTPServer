cmake_minimum_required(VERSION 3.10)
project(ChatServer)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)
find_package(redis++ REQUIRED)

set(HTTP_SERVER_ROOT "${CMAKE_SOURCE_DIR}/../HttpServer")

include_directories(
    ${HTTP_SERVER_ROOT}/include
)

file(GLOB_RECURSE HTTP_SERVER_SRCS
    "${HTTP_SERVER_ROOT}/src/*.cpp"
)

add_executable(chat_server
    chat_main.cpp
    ${HTTP_SERVER_SRCS}
)

target_link_libraries(chat_server
    muduo_net
    muduo_base
    pthread
    ssl
    crypto
    mysqlcppconn
    CURL::libcurl
    redis++::redis++
)

add_custom_command(TARGET chat_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_SOURCE_DIR}/chat_ui.html
        $<TARGET_FILE_DIR:chat_server>/chat_ui.html
)
