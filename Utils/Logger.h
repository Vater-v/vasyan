#pragma once
#include <android/log.h>

#define LOG_TAG "Hmuriy"

// Добавил проверку, чтобы не было конфликтов имен
#ifndef LOGI
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#endif

#ifndef LOGW
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#endif

#ifndef LOGE
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#endif