cmake_minimum_required(VERSION 3.10)
project(ServerTCP)

# Указываем директорию с исходным кодом
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "-g -Wall -Wextra")
include_directories(src)

# Добавляем файлы исходного кода
add_executable(server src/server.cpp)
