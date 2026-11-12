#pragma once
#include <stdio.h>
// Quiet unless HOST_TEST_VERBOSE is defined; the tag is always used.
#ifdef HOST_TEST_VERBOSE
#define HOST_LOG(level, tag, ...) (printf("%s %s: ", level, tag), printf(__VA_ARGS__), printf("\n"))
#else
#define HOST_LOG(level, tag, ...) ((void)(tag))
#endif
#define ESP_LOGE(tag, ...) HOST_LOG("E", tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) HOST_LOG("W", tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) HOST_LOG("I", tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) HOST_LOG("D", tag, __VA_ARGS__)
