#pragma once

#ifdef USE_ESP32
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

namespace esphome {
namespace snapclient {

#define HTTP_TASK_PRIORITY (configMAX_PRIORITIES - 2)  // 9
#define HTTP_TASK_CORE_ID 1                            // 1  // tskNO_AFFINITY

#define FLAC_DECODER_TASK_PRIORITY 7
#define FLAC_DECODER_TASK_CORE_ID tskNO_AFFINITY
// HTTP_TASK_CORE_ID  // 1  // tskNO_AFFINITY

#define FLAC_TASK_PRIORITY 8
#define FLAC_TASK_CORE_ID tskNO_AFFINITY

#define OPUS_TASK_PRIORITY 8
#define OPUS_TASK_CORE_ID tskNO_AFFINITY

// 1  // tskNO_AFFINITY

typedef struct audioDACdata_s {
  bool mute;
  int volume;
} audioDACdata_t;

void http_get_task(void *pvParameters);
void init_snapcast(QueueHandle_t audioQHdl);
extern "C" void audio_set_mute(bool mute);
void audio_set_volume(int volume);


}  // namespace snapclient
}  // namespace esphome

#endif