cmake_minimum_required(VERSION 3.10.2)

project("Vasyan Hmuriy")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(
    -Wno-deprecated-declarations 
    -Wno-non-portable-include-path
)

# Добавляем папку Network в пути поиска заголовков
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/Utils
    ${CMAKE_CURRENT_SOURCE_DIR}/Network
)

set(SOURCE_FILES
    main.cpp
    Utils/And64InlineHook.cpp
    Utils/Il2Cpp.cpp
    Utils/Utils.cpp
    Utils/Spoofer.cpp
    # Перенесенные и новые файлы
    Network/NetworkSender.cpp
    Network/TrafficMonitor.cpp
)

add_library(
    hmuriy
    SHARED
    ${SOURCE_FILES}
)

find_library(log-lib log)

target_link_libraries(
    hmuriy
    ${log-lib}
)