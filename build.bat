@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul
title Vasyan Builder [Fixed Filename]

:: ==========================================
:: 1. КОНФИГУРАЦИЯ
:: ==========================================

:: Путь к NDK
set "NDK_PATH=C:\android-ndk-r29"

:: Определяем корень
set "CURRENT_DIR=%~dp0"
pushd "%CURRENT_DIR%.."
set "ROOT_DIR=%CD%"
popd

:: Инструменты
set "APKTOOL=%ROOT_DIR%\apktool.jar"
set "SIGNER=%ROOT_DIR%\uber-apk-signer.jar"
set "UNPACKED_GAME=%ROOT_DIR%\game"
set "BUILD_DIR=%CURRENT_DIR%cmake_build"

:: Файлы
set "LIB_NAME=libhmuriy.so"
set "LIB_DEST_FOLDER=%UNPACKED_GAME%\lib\arm64-v8a"

:: Базовое имя для временных файлов (чтобы не путаться)
set "TEMP_BASE_NAME=game-temp"
set "UNSIGNED_APK=%ROOT_DIR%\%TEMP_BASE_NAME%-unsigned.apk"

:: UberSigner убирает "-unsigned" и добавляет это:
set "SIGNED_SUFFIX=-aligned-debugSigned"
set "FINAL_APK_NAME=game-patched.apk"
set "FINAL_APK_PATH=%ROOT_DIR%\%FINAL_APK_NAME%"

:: ==========================================
:: 2. ОЧИСТКА
:: ==========================================
echo [0/5] Cleaning workspace...
if exist "%BUILD_DIR%" rd /s /q "%BUILD_DIR%"
if exist "%UNSIGNED_APK%" del "%UNSIGNED_APK%"
if exist "%FINAL_APK_PATH%" del "%FINAL_APK_PATH%"
:: Чистим любые промежуточные файлы подписи
del "%ROOT_DIR%\*%SIGNED_SUFFIX%.apk" 2>nul
if exist "%ROOT_DIR%\idsig" rd /s /q "%ROOT_DIR%\idsig"

if not exist "%UNPACKED_GAME%" (
    echo [ERROR] Папка "%UNPACKED_GAME%" не найдена! Сначала распакуй APK.
    pause
    exit /b
)

:: ==========================================
:: 3. СБОРКА C++ (CMake)
:: ==========================================
echo.
echo [1/5] Compiling libhmuriy.so...
echo ----------------------------------

cmake -B "%BUILD_DIR%" -G "Ninja" ^
    -DCMAKE_TOOLCHAIN_FILE="%NDK_PATH%\build\cmake\android.toolchain.cmake" ^
    -DANDROID_ABI="arm64-v8a" ^
    -DANDROID_PLATFORM=android-24 ^
    -DCMAKE_BUILD_TYPE=Release

if %errorlevel% neq 0 goto :error

cmake --build "%BUILD_DIR%"
if %errorlevel% neq 0 goto :error

:: ==========================================
:: 4. КОПИРОВАНИЕ БИБЛИОТЕКИ
:: ==========================================
echo.
echo [2/5] Injecting .so into game folder...
echo ----------------------------------

set "SOURCE_LIB=%BUILD_DIR%\%LIB_NAME%"

if not exist "!SOURCE_LIB!" (
    set "SOURCE_LIB=%BUILD_DIR%\lib\%LIB_NAME%"
)

if not exist "!SOURCE_LIB!" (
    echo [FATAL ERROR] libhmuriy.so не найден в папке сборки!
    goto :error
)

if not exist "%LIB_DEST_FOLDER%" mkdir "%LIB_DEST_FOLDER%"

echo Copying: "!SOURCE_LIB!" 
echo To:      "%LIB_DEST_FOLDER%\%LIB_NAME%"
copy /Y "!SOURCE_LIB!" "%LIB_DEST_FOLDER%\%LIB_NAME%" >nul

if not exist "%LIB_DEST_FOLDER%\%LIB_NAME%" (
    echo [ERROR] Ошибка копирования. Файл не появился.
    goto :error
)

:: ==========================================
:: 5. СБОРКА APK
:: ==========================================
echo.
echo [3/5] Building APK...
echo ----------------------------------

java -jar "%APKTOOL%" b "%UNPACKED_GAME%" -o "%UNSIGNED_APK%"
if %errorlevel% neq 0 (
    echo [ERROR] Apktool failed.
    goto :error
)

:: ==========================================
:: 6. ПОДПИСЬ
:: ==========================================
echo.
echo [4/5] Signing APK...
echo ----------------------------------

java -jar "%SIGNER%" -a "%UNSIGNED_APK%" --allowResign
if %errorlevel% neq 0 (
    echo [ERROR] Signing failed.
    goto :error
)

:: === ФИКС ЗДЕСЬ ===
:: UberSigner вырезал слово "-unsigned" из имени файла.
:: Было: game-temp-unsigned.apk
:: Стало: game-temp-aligned-debugSigned.apk
set "UBER_OUTPUT=%ROOT_DIR%\%TEMP_BASE_NAME%%SIGNED_SUFFIX%.apk"

if not exist "%UBER_OUTPUT%" (
    echo [ERROR] Signed file not found: %UBER_OUTPUT%
    goto :error
)

:: Переименовываем в итоговый файл
move /Y "%UBER_OUTPUT%" "%FINAL_APK_PATH%" >nul
echo [INFO] Renamed to: %FINAL_APK_NAME%

:: ==========================================
:: 7. ФИНАЛЬНАЯ УБОРКА
:: ==========================================
echo.
echo [5/5] Cleaning up temp files...
echo ----------------------------------

if exist "%BUILD_DIR%" rd /s /q "%BUILD_DIR%"
if exist "%UNSIGNED_APK%" del "%UNSIGNED_APK%"
if exist "%ROOT_DIR%\idsig" rd /s /q "%ROOT_DIR%\idsig"

echo.
echo ==========================================
echo [SUCCESS] DONE!
echo File: %FINAL_APK_PATH%
echo ==========================================

:: ==========================================
:: 8. УСТАНОВКА
:: ==========================================
echo.
echo Press ENTER to Install via ADB
pause >nul

adb install -r "%FINAL_APK_PATH%"
if %errorlevel% neq 0 (
    echo [ERROR] Install failed.
) else (
    echo [INFO] Installed! Launching logs...
    echo.
    adb logcat -s Hmuriy:V DEBUG:V AndroidRuntime:E
)

pause
exit /b

:error
echo.
echo [FAIL] Script stopped due to errors.
pause
exit /b