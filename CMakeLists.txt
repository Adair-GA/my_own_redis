cmake_minimum_required(VERSION 3.28)
project(my_own_redis_cpp)

set(CMAKE_CXX_STANDARD 20)

add_executable(my_own_redis_server_cpp server.cpp
        utils/utils.cpp
        utils/utils.h)

add_executable(my_own_redis_client_cpp
        client.cpp)
