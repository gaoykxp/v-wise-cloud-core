cmake_minimum_required(VERSION 3.10)
project(vwise_cloud_core CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3 /utf-8)
else()
    add_compile_options(-Wall -Wextra)
endif()

set(SRCS
    src/net.cpp
    src/mini_mqtt.cpp
    src/cloud_config.cpp
    src/anchor_server.cpp
    src/downlink_sender.cpp
    src/cloud_server.cpp
    src/web_dashboard.cpp
    src/selftest.cpp
    src/main.cpp
)

add_executable(vwise_cloud_core ${SRCS})
target_include_directories(vwise_cloud_core PRIVATE src)

if(WIN32)
    target_link_libraries(vwise_cloud_core PRIVATE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(vwise_cloud_core PRIVATE Threads::Threads)
endif()

# 部署：www/ 静态看板 + cloud.ini 拷贝到运行目录（构建目录 / 安装目录）
add_custom_command(TARGET vwise_cloud_core POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/www $<TARGET_FILE_DIR:vwise_cloud_core>/www
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/cloud.ini $<TARGET_FILE_DIR:vwise_cloud_core>/cloud.ini)

install(TARGETS vwise_cloud_core RUNTIME DESTINATION bin)
install(DIRECTORY www DESTINATION bin)
install(FILES cloud.ini DESTINATION bin)
