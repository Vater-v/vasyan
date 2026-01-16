cmake_minimum_required(VERSION 3.10.2)

# Имя проекта
project("Vasyan Hmuriy")

# ==========================================
# 1. Настройки компилятора
# ==========================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(
    -Wno-deprecated-declarations 
    -Wno-non-portable-include-path
)

# ==========================================
# 2. Определение исходников
# ==========================================
set(SOURCE_FILES
    main.cpp
    utils/And64InlineHook.cpp
    utils/Il2Cpp.cpp
    utils/Utils.cpp
    utils/NetworkSender.cpp  # <--- Добавили наш новый сендер
)

# ==========================================
# 3. Создание библиотеки
# ==========================================
add_library(
    hmuriy
    SHARED
    ${SOURCE_FILES}
)

# ==========================================
# 4. Настройка путей
# ==========================================
target_include_directories(hmuriy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/utils
)

# ==========================================
# 5. Линковка
# ==========================================
find_library(log-lib log)

target_link_libraries(
    hmuriy
    ${log-lib}
)