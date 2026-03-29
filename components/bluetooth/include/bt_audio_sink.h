#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_DELAY_VALUE 200 /* in ms, delay caused by application layer */

void bt_audio_sink_init(i2s_port_t i2sN, i2s_std_gpio_config_t pin_conf, void (*set_mute)(bool, bool), bool (*lock)(bool, TickType_t));
void bt_audio_sink_start();
void bt_audio_sink_stop();
bool bt_audio_sink_is_connected();

#ifdef __cplusplus
}
#endif
