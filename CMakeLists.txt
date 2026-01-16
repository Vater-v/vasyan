cmake_minimum_required(VERSION 3.10.2)

project("Vasyan Hmuriy")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(
    -Wno-deprecated-declarations 
    -Wno-non-portable-include-path
)

set(SOURCE_FILES
    main.cpp
    utils/And64InlineHook.cpp
    utils/Il2Cpp.cpp
    utils/Utils.cpp
    utils/NetworkSender.cpp
    utils/Spoofer.cpp       # <--- ДОБАВЛЕНО
)

add_library(
    hmuriy
    SHARED
    ${SOURCE_FILES}
)

target_include_directories(hmuriy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
)

find_library(log-lib log)

target_link_libraries(
    hmuriy
    ${log-lib}
)