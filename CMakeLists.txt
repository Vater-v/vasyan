cmake_minimum_required(VERSION 3.10.2)

# Имя проекта
project("Vasyan Hmuriy")

# ==========================================
# 1. Настройки компилятора
# ==========================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Флаги компиляции:
# -Wno-deprecated-declarations: Игнорировать устаревшие функции
# -Wno-non-portable-include-path: Игнорировать разницу регистра (Utils vs utils) на Windows
add_compile_options(
    -Wno-deprecated-declarations 
    -Wno-non-portable-include-path
)

# ==========================================
# 2. Определение исходников
# ==========================================
# Собираем список файлов для удобства
set(SOURCE_FILES
    main.cpp
    utils/And64InlineHook.cpp
    # Если в utils есть .c файлы (например, реализация хука на C), добавь их сюда:
    # utils/And64InlineHook.c 
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
# 4. Настройка путей (Includes)
# ==========================================
# PRIVATE означает, что эти пути нужны только для сборки этой цели
target_include_directories(hmuriy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}        # Чтобы работало #include "utils/..."
    ${CMAKE_CURRENT_SOURCE_DIR}/utils  # Чтобы работало #include "And64InlineHook.hpp"
)

# ==========================================
# 5. Линковка (Logcat)
# ==========================================
find_library(log-lib log)

target_link_libraries(
    hmuriy
    ${log-lib}
)