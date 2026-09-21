#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Opus decoder is implemented as a subcomponet from master git repo
#include "opus.h"

// flac decoder is implemented as a subcomponet from master git repo
#include "FLAC/stream_decoder.h"

#if CONFIG_USE_DSP_PROCESSOR
#include "dsp_processor.h"
#endif

#include "connection_handler.h"
#include "player.h"
#ifdef HAS_SETTINGS_MANAGER
#include "settings_manager.h"
#endif
#include "snapcast.h"
#include "snapcast_protocol_parser.h"
#include "snapclient_helper.h"

static bool isCachedChunk = false;
static uint32_t cachedBlocks = 0;

static FLAC__StreamDecoderReadStatus read_callback(
    const FLAC__StreamDecoder* decoder, FLAC__byte buffer[], size_t* bytes,
    void* client_data);
static FLAC__StreamDecoderWriteStatus write_callback(
    const FLAC__StreamDecoder* decoder, const FLAC__Frame* frame,
    const FLAC__int32* const buffer[], void* client_data);
static void metadata_callback(const FLAC__StreamDecoder* decoder,
                              const FLAC__StreamMetadata* metadata,
                              void* client_data);
static void error_callback(const FLAC__StreamDecoder* decoder,
                           FLAC__StreamDecoderErrorStatus status,
                           void* client_data);

static FLAC__StreamDecoder* flacDecoder = NULL;

const char* VERSION_STRING = "0.0.3";

#define HTTP_TASK_PRIORITY 17
#define HTTP_TASK_CORE_ID 1

TaskHandle_t t_http_get_task = NULL;

#define FAST_SYNC_LATENCY_BUF 10000      // in µs
#define NORMAL_SYNC_LATENCY_BUF 1000000  // in µs

/* snapast parameters; configurable in menuconfig */
#define SNAPCAST_USE_SOFT_VOL CONFIG_SNAPCLIENT_USE_SOFT_VOL

/* Logging tag */
static const char* TAG = "SC";

// static QueueHandle_t playerChunkQueueHandle = NULL;
SemaphoreHandle_t timeSyncSemaphoreHandle = NULL;

SemaphoreHandle_t idCounterSemaphoreHandle = NULL;

static void (*set_volume_cb)(int volume);
static void (*set_mute_cb)(bool mute);

void time_sync_msg_cb(void* args);

static char base_message_serialized[BASE_MESSAGE_SIZE];

// static const esp_timer_create_args_t tSyncArgs = {
//     .callback = &time_sync_msg_cb,
//     .dispatch_method = ESP_TIMER_TASK,
//     .name = "tSyncMsg",
//     .skip_unhandled_events = false};

struct netconn* lwipNetconn;

static int id_counter = 0;

static OpusDecoder* opusDecoder = NULL;

static decoderData_t decoderChunk = {
    .type = SNAPCAST_MESSAGE_INVALID,
    .inData = NULL,
    .timestamp = {0, 0},
    .outData = NULL,
    .bytes = 0,
};

static decoderData_t pcmChunk = {
    .type = SNAPCAST_MESSAGE_INVALID,
    .inData = NULL,
    .timestamp = {0, 0},
    .outData = NULL,
    .bytes = 0,
};

/**
 *
 */
void time_sync_msg_cb(void* args) {
  base_message_t base_message_tx;
  //  struct timeval now;
  int64_t now;
  int rc1;
  uint8_t p_pkt[BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE];

  //  uint8_t *p_pkt = (uint8_t *)malloc(BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE);
  //  if (p_pkt == NULL) {
  //    ESP_LOGW(
  //        TAG,
  //        "%s: Failed to get memory for time sync message. Skipping this
  //        round.",
  //        __func__);
  //
  //    return;
  //  }

  memset(p_pkt, 0, BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE);

  base_message_tx.type = SNAPCAST_MESSAGE_TIME;

  xSemaphoreTake(idCounterSemaphoreHandle, portMAX_DELAY);
  base_message_tx.id = id_counter++;
  xSemaphoreGive(idCounterSemaphoreHandle);

  base_message_tx.refersTo = 0;
  base_message_tx.received.sec = 0;
  base_message_tx.received.usec = 0;
  now = esp_timer_get_time();
  base_message_tx.sent.sec = now / 1000000;
  base_message_tx.sent.usec = now - base_message_tx.sent.sec * 1000000;
  base_message_tx.size = TIME_MESSAGE_SIZE;
  rc1 = base_message_serialize(&base_message_tx, (char*)&p_pkt[0],
                               BASE_MESSAGE_SIZE);
  if (rc1) {
    ESP_LOGE(TAG, "Failed to serialize base message for time");

    return;
  }

  rc1 = netconn_write(lwipNetconn, p_pkt, BASE_MESSAGE_SIZE + TIME_MESSAGE_SIZE,
                      NETCONN_NOCOPY);

  if (rc1 != ERR_OK) {
    ESP_LOGW(TAG, "error writing timesync msg");

    return;
  }

  //  free(p_pkt);

  // ESP_LOGI(TAG, "%s: sent time sync message, %u", __func__,
  // base_message_tx.id);
}

typedef struct {
  int64_t now;
  int64_t lastTimeSync;
  int64_t lastTimeSyncSent;
  uint64_t timeout;
} time_sync_data_t;

/**
 *
 */
void time_sync_msg_received(base_message_t* base_message_rx,
                            time_message_t* time_message_rx,
                            time_sync_data_t* time_sync_data,
                            bool received_codec_header) {
  int64_t tmpDiffToServer, trx, tdif, ttx, diff;
  trx = (int64_t)base_message_rx->received.sec * 1000000LL +
        (int64_t)base_message_rx->received.usec;
  ttx = (int64_t)base_message_rx->sent.sec * 1000000LL +
        (int64_t)base_message_rx->sent.usec;
  tdif = trx - ttx;  // T4-T3
  ttx = (int64_t)time_message_rx->latency.sec * 1000000LL +
        (int64_t)time_message_rx->latency.usec;  // T2-T1
  tmpDiffToServer = (ttx - tdif) / 2;            //((T2-T1) - (-T3+T4))/2

  // clear diffBuffer if last update is
  // older than a minute
  diff = time_sync_data->now - time_sync_data->lastTimeSync;
  if (diff > 60000000LL) {
    ESP_LOGW(TAG,
             "Last time sync older "
             "than a minute. "
             "Clearing time buffer");

    reset_latency_buffer();

    time_sync_data->timeout = FAST_SYNC_LATENCY_BUF;

    netconn_set_recvtimeout(lwipNetconn,
                            time_sync_data->timeout / 1000);  // timeout in ms

    // esp_timer_stop(time_sync_data->timeSyncMessageTimer);
    // if (received_codec_header == true) {
    //   if (!esp_timer_is_active(time_sync_data->timeSyncMessageTimer)) {
    //     esp_timer_start_periodic(time_sync_data->timeSyncMessageTimer,
    //                              time_sync_data->timeout);
    //   }
    // }
  }

#if USE_TIMEFILTER
  player_latency_insert(tmpDiffToServer, (tdif + ttx) / 2, trx);
#else
  player_latency_insert(tmpDiffToServer);
#endif

  // ESP_LOGI(TAG, "Current latency:%lld:",
  // tmpDiffToServer);

  // store current time
  time_sync_data->lastTimeSync = time_sync_data->now;

  if (received_codec_header == true) {
    // if (!esp_timer_is_active(time_sync_data->timeSyncMessageTimer)) {
    //   esp_timer_start_periodic(time_sync_data->timeSyncMessageTimer,
    //                            time_sync_data->timeout);
    // }

    bool is_full = false;
    latency_buffer_full(&is_full);
    if ((is_full == true) &&
        (time_sync_data->timeout < NORMAL_SYNC_LATENCY_BUF)) {
      time_sync_data->timeout = NORMAL_SYNC_LATENCY_BUF;
      netconn_set_recvtimeout(lwipNetconn,
                              time_sync_data->timeout / 1000);  // timeout in ms

      ESP_LOGI(TAG, "latency buffer full");

      // if (esp_timer_is_active(time_sync_data->timeSyncMessageTimer)) {
      //   esp_timer_stop(time_sync_data->timeSyncMessageTimer);
      // }

      // esp_timer_start_periodic(time_sync_data->timeSyncMessageTimer,
      //                          time_sync_data->timeout);
    } else if ((is_full == false) &&
               (time_sync_data->timeout > FAST_SYNC_LATENCY_BUF)) {
      time_sync_data->timeout = FAST_SYNC_LATENCY_BUF;
      netconn_set_recvtimeout(lwipNetconn,
                              time_sync_data->timeout / 1000);  // timeout in ms

      ESP_LOGI(TAG, "latency buffer not full");

      // if (esp_timer_is_active(time_sync_data->timeSyncMessageTimer)) {
      //   esp_timer_stop(time_sync_data->timeSyncMessageTimer);
      // }

      // esp_timer_start_periodic(time_sync_data->timeSyncMessageTimer,
      //                          time_sync_data->timeout);
    }
  }
}

/**
 *
 */
static FLAC__StreamDecoderReadStatus read_callback(
    const FLAC__StreamDecoder* decoder, FLAC__byte buffer[], size_t* bytes,
    void* client_data) {
  snapcastSetting_t* scSet = (snapcastSetting_t*)client_data;
  //  decoderData_t *flacData;

  (void)scSet;

  // xQueueReceive(decoderReadQHdl, &flacData, portMAX_DELAY);
  // if (xQueueReceive(decoderReadQHdl, &flacData, pdMS_TO_TICKS(100)))
  if (decoderChunk.inData) {
    //	   ESP_LOGI(TAG, "in flac read cb %ld %p", flacData->bytes,
    // flacData->inData);

    if (decoderChunk.bytes <= 0) {
      //	    free_flac_data(flacData);

      return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
    }

    isCachedChunk = false;

    //	  if (flacData->inData == NULL) {
    //	    free_flac_data(flacData);
    //
    //	    return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    //	  }

    if (decoderChunk.bytes <= *bytes) {
      memcpy(buffer, decoderChunk.inData, decoderChunk.bytes);
      *bytes = decoderChunk.bytes;

      // ESP_LOGW(TAG, "read all flac inData %d", *bytes);

      free(decoderChunk.inData);
      decoderChunk.inData = NULL;
      decoderChunk.bytes = 0;
    } else {
      memcpy(buffer, decoderChunk.inData, *bytes);

      memmove(decoderChunk.inData, decoderChunk.inData + *bytes,
              decoderChunk.bytes - *bytes);
      decoderChunk.bytes -= *bytes;
      decoderChunk.inData =
          (uint8_t*)realloc(decoderChunk.inData, decoderChunk.bytes);

      // ESP_LOGW(TAG, "didn't read all flac inData %d", *bytes);
      //	    flacData->inData += *bytes;
      //	    flacData->bytes -= *bytes;
    }

    // free_flac_data(flacData);

    // xQueueSend (flacReadQHdl, &flacData, portMAX_DELAY);

    // xSemaphoreGive(decoderReadSemaphore);

    // ESP_LOGE(TAG, "%s: data processed", __func__);

    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
  } else {
    return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
  }
}

/**
 *
 */
static FLAC__StreamDecoderWriteStatus write_callback(
    const FLAC__StreamDecoder* decoder, const FLAC__Frame* frame,
    const FLAC__int32* const buffer[], void* client_data) {
  size_t i;
  snapcastSetting_t* scSet = (snapcastSetting_t*)client_data;

  size_t bytes = frame->header.blocksize * frame->header.channels *
                 frame->header.bits_per_sample / 8;

  (void)decoder;

  if (isCachedChunk) {
    cachedBlocks += frame->header.blocksize;
  }

  //  ESP_LOGI(TAG, "in flac write cb %ld %d, pcmChunk.bytes %ld",
  //  frame->header.blocksize, bytes, pcmChunk.bytes);

  if (frame->header.channels != scSet->ch) {
    ESP_LOGE(TAG,
             "ERROR: frame header reports different channel count %ld than "
             "previous metadata block %d",
             frame->header.channels, scSet->ch);
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (frame->header.bits_per_sample != scSet->bits) {
    ESP_LOGE(TAG,
             "ERROR: frame header reports different bps %ld than previous "
             "metadata block %d",
             frame->header.bits_per_sample, scSet->bits);
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (buffer[0] == NULL) {
    ESP_LOGE(TAG, "ERROR: buffer [0] is NULL");
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }
  if (buffer[1] == NULL) {
    ESP_LOGE(TAG, "ERROR: buffer [1] is NULL");
    return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
  }

  uint8_t* pcmData;
  do {
    pcmData = (uint8_t*)realloc(pcmChunk.outData, pcmChunk.bytes + bytes);
    if (!pcmData) {
      ESP_LOGW(TAG,
               "%s, failed to allocate PCM chunk payload (%lu + %u bytes, free "
               "heap %u, largest block %u), try again",
               __func__, pcmChunk.bytes, bytes,
               heap_caps_get_free_size(MALLOC_CAP_8BIT),
               heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
      vTaskDelay(pdMS_TO_TICKS(5));
      // return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    }
#if 0  // enable heap usage profiling
    else {
      static size_t largestFreeBlockMin = 10000000;
      size_t largestFreeBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
      static size_t freeSizeMin = 10000000;
      if (largestFreeBlock < largestFreeBlockMin) {
        largestFreeBlockMin = largestFreeBlock;
        ESP_LOGI(TAG, "%s, free heap %u, largest block %u", __func__,
                                                            heap_caps_get_free_size(MALLOC_CAP_8BIT),
                                                            largestFreeBlockMin);
      }
    }
#endif
  } while (!pcmData);

  pcmChunk.outData = pcmData;

  for (i = 0; i < frame->header.blocksize; i++) {
    // write little endian
    pcmChunk.outData[pcmChunk.bytes + 4 * i] = (uint8_t)(buffer[0][i]);
    pcmChunk.outData[pcmChunk.bytes + 4 * i + 1] = (uint8_t)(buffer[0][i] >> 8);
    pcmChunk.outData[pcmChunk.bytes + 4 * i + 2] = (uint8_t)(buffer[1][i]);
    pcmChunk.outData[pcmChunk.bytes + 4 * i + 3] = (uint8_t)(buffer[1][i] >> 8);
  }

  pcmChunk.bytes += bytes;

  scSet->chkInFrames = frame->header.blocksize;

  return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

/**
 *
 */
void metadata_callback(const FLAC__StreamDecoder* decoder,
                       const FLAC__StreamMetadata* metadata,
                       void* client_data) {
  snapcastSetting_t* scSet = (snapcastSetting_t*)client_data;

  (void)decoder;

  if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
    // ESP_LOGI(TAG, "in flac meta cb");

    // save for later
    scSet->sr = metadata->data.stream_info.sample_rate;
    scSet->ch = metadata->data.stream_info.channels;
    scSet->bits = metadata->data.stream_info.bits_per_sample;

    ESP_LOGI(TAG, "fLaC sampleformat: %ld:%d:%d", scSet->sr, scSet->bits,
             scSet->ch);

    // ESP_LOGE(TAG, "%s: data processed", __func__);
  }
}

/**
 *
 */
void error_callback(const FLAC__StreamDecoder* decoder,
                    FLAC__StreamDecoderErrorStatus status, void* client_data) {
  (void)decoder, (void)client_data;

  ESP_LOGE(TAG, "Got error callback: %s\n",
           FLAC__StreamDecoderErrorStatusString[status]);
}

/**
 *
 */
void server_settings_msg_received(
    server_settings_message_t* server_settings_message,
    snapcastSetting_t* scSet) {
  static int volume = 0;
  // log mute state, buffer, latency
  ESP_LOGI(TAG, "Buffer length:  %ld", server_settings_message->buffer_ms);
  ESP_LOGI(TAG, "Latency:        %ld", server_settings_message->latency);
  ESP_LOGI(TAG, "Mute:           %d", server_settings_message->muted);
  ESP_LOGI(TAG, "Setting volume: %ld", server_settings_message->volume);

  // Volume setting using ADF HAL
  // abstraction
  if (scSet->muted != server_settings_message->muted) {
#if SNAPCAST_USE_SOFT_VOL
    if (server_settings_message->muted) {
      dsp_processor_set_volome(0.0);
    } else {
      dsp_processor_set_volome((double)server_settings_message->volume / 100);
    }
#endif
    set_mute_cb(server_settings_message->muted);
  }

  if (volume != server_settings_message->volume) {
#if SNAPCAST_USE_SOFT_VOL
    if (!server_settings_message->muted) {
      dsp_processor_set_volome((double)server_settings_message->volume / 100);
    }
#else
    set_volume_cb(server_settings_message->volume);
#endif
  }

  scSet->muted = server_settings_message->muted;
  volume = server_settings_message->volume;

  if (scSet->cDacLat_ms != server_settings_message->latency ||
      scSet->buf_ms != server_settings_message->buffer_ms) {
    scSet->cDacLat_ms = server_settings_message->latency;
    scSet->buf_ms = server_settings_message->buffer_ms;

    if (player_send_snapcast_setting(scSet) != pdPASS) {
      ESP_LOGE(TAG,
               "Failed to notify sync task. "
               "Did you init player?");

      esp_restart();
    }
  }
}

/**
 *
 */
void codec_header_received(char* codecPayload, uint32_t codecPayloadLen,
                           codec_type_t codec, snapcastSetting_t* scSet,
                           time_sync_data_t* time_sync_data) {
  // first ensure everything is set up
  // correctly and resources are
  // available

  if (flacDecoder != NULL) {
    FLAC__stream_decoder_finish(flacDecoder);
    FLAC__stream_decoder_delete(flacDecoder);
    flacDecoder = NULL;
  }

  if (opusDecoder != NULL) {
    opus_decoder_destroy(opusDecoder);
    opusDecoder = NULL;
  }

  if (codec == OPUS) {
    uint16_t channels;
    uint32_t rate;
    uint16_t bits;

    memcpy(&rate, codecPayload + 4, sizeof(rate));
    memcpy(&bits, codecPayload + 8, sizeof(bits));
    memcpy(&channels, codecPayload + 10, sizeof(channels));

    scSet->codec = codec;
    scSet->bits = bits;
    scSet->ch = channels;
    scSet->sr = rate;

    ESP_LOGI(TAG, "Opus sample format: %ld:%d:%d\n", rate, bits, channels);

    int error = 0;

    opusDecoder = opus_decoder_create(scSet->sr, scSet->ch, &error);
    if (error != 0) {
      ESP_LOGI(TAG, "Failed to init opus coder");
      esp_restart();
    }

    ESP_LOGI(TAG, "Initialized opus Decoder: %d", error);
  } else if (codec == FLAC) {
    decoderChunk.bytes = codecPayloadLen;
    do {
      decoderChunk.inData = (uint8_t*)malloc(decoderChunk.bytes);
      vTaskDelay(pdMS_TO_TICKS(1));
    } while (decoderChunk.inData == NULL);
    memcpy(decoderChunk.inData, codecPayload, codecPayloadLen);
    decoderChunk.outData = NULL;
    decoderChunk.type = SNAPCAST_MESSAGE_CODEC_HEADER;

    flacDecoder = FLAC__stream_decoder_new();
    if (flacDecoder == NULL) {
      ESP_LOGE(TAG, "Failed to init flac decoder");
      esp_restart();
    }

    FLAC__StreamDecoderInitStatus init_status =
        FLAC__stream_decoder_init_stream(
            flacDecoder, read_callback, NULL, NULL, NULL, NULL, write_callback,
            metadata_callback, error_callback, scSet);
    if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
      ESP_LOGE(TAG, "ERROR: initializing decoder: %s\n",
               FLAC__StreamDecoderInitStatusString[init_status]);

      esp_restart();
    }

    FLAC__stream_decoder_process_until_end_of_metadata(flacDecoder);

    // ESP_LOGI(TAG, "%s: processed codec header",
    // __func__);
  } else if (codec == PCM) {
    uint16_t channels;
    uint32_t rate;
    uint16_t bits;

    memcpy(&channels, codecPayload + 22, sizeof(channels));
    memcpy(&rate, codecPayload + 24, sizeof(rate));
    memcpy(&bits, codecPayload + 34, sizeof(bits));

    scSet->codec = codec;
    scSet->bits = bits;
    scSet->ch = channels;
    scSet->sr = rate;

    ESP_LOGI(TAG, "pcm sampleformat: %ld:%d:%d", scSet->sr, scSet->bits,
             scSet->ch);
  } else {
    ESP_LOGE(TAG,
             "codec header decoder "
             "shouldn't get here after "
             "codec string was detected");

    esp_restart();
  }

  if (player_send_snapcast_setting(scSet) != pdPASS) {
    ESP_LOGE(TAG,
             "Failed to notify sync task. "
             "Did you init player?");

    esp_restart();
  }

  // ESP_LOGI(TAG, "done codec header msg");

  // esp_timer_stop(time_sync_data->timeSyncMessageTimer);
  // if (!esp_timer_is_active(time_sync_data->timeSyncMessageTimer)) {
  //   esp_timer_start_periodic(time_sync_data->timeSyncMessageTimer,
  //                            time_sync_data->timeout);
  // }
}

/**
 *
 */
void handle_chunk_message(codec_type_t codec, snapcastSetting_t* scSet,
                          pcm_chunk_message_t** pcmData,
                          wire_chunk_message_t* wire_chnk) {
  switch (codec) {
    case OPUS: {
      int frame_size = -1;
      int samples_per_frame;
      opus_int16* audio = NULL;

      samples_per_frame =
          opus_packet_get_samples_per_frame(decoderChunk.inData, scSet->sr);
      if (samples_per_frame < 0) {
        ESP_LOGE(TAG,
                 "couldn't get samples per frame count "
                 "of packet");
      }

      scSet->chkInFrames = samples_per_frame;

      // ESP_LOGW(TAG, "%d, %llu, %llu",
      // samples_per_frame, 1000000ULL *
      // samples_per_frame / scSet->sr,
      // 1000000ULL *
      // wire_chnk->timestamp.sec +
      // wire_chnk->timestamp.usec);

      // ESP_LOGW(TAG, "got OPUS decoded chunk size: %ld
      // " "frames from encoded chunk with size %d,
      // allocated audio buffer %d", scSet->chkInFrames,
      // wire_chnk->size, samples_per_frame);

      size_t bytes;
      do {
        bytes = samples_per_frame * (scSet->ch * scSet->bits >> 3);

        while ((audio = (opus_int16*)realloc(audio, bytes)) == NULL) {
          ESP_LOGE(TAG,
                   "couldn't realloc memory for OPUS "
                   "audio %d",
                   bytes);

          vTaskDelay(pdMS_TO_TICKS(1));
        }

        frame_size =
            opus_decode(opusDecoder, decoderChunk.inData, decoderChunk.bytes,
                        (opus_int16*)audio, samples_per_frame, 0);

        samples_per_frame <<= 1;
      } while (frame_size < 0);

      free(decoderChunk.inData);
      decoderChunk.inData = NULL;

      pcm_chunk_message_t* new_pcmChunk = NULL;

      // ESP_LOGW(TAG, "OPUS decode: %d", frame_size);

      if (allocate_pcm_chunk_memory(&new_pcmChunk, bytes) < 0) {
        *pcmData = NULL;
      } else {
        new_pcmChunk->timestamp = wire_chnk->timestamp;

        if (new_pcmChunk->fragment->payload) {
          volatile uint32_t* sample;
          uint32_t tmpData;
          uint32_t cnt = 0;

          for (int i = 0; i < bytes; i += 4) {
            sample =
                (volatile uint32_t*)(&(new_pcmChunk->fragment->payload[i]));
            tmpData = (((uint32_t)audio[cnt] << 16) & 0xFFFF0000) |
                      (((uint32_t)audio[cnt + 1] << 0) & 0x0000FFFF);
            *sample = (volatile uint32_t)tmpData;

            cnt += 2;
          }
        }

        free(audio);
        audio = NULL;

#if CONFIG_USE_DSP_PROCESSOR
        if (new_pcmChunk->fragment->payload) {
          dsp_processor_worker((void*)new_pcmChunk, (void*)scSet);
        }
#endif

        insert_pcm_chunk(new_pcmChunk);
      }

      if (player_send_snapcast_setting(scSet) != pdPASS) {
        ESP_LOGE(TAG,
                 "Failed to notify "
                 "sync task about "
                 "codec. Did you "
                 "init player?");

        esp_restart();
      }

      break;
    }

    case FLAC: {
      isCachedChunk = true;
      cachedBlocks = 0;

      while (decoderChunk.bytes > 0) {
        if (FLAC__stream_decoder_process_single(flacDecoder) == 0) {
          ESP_LOGE(TAG,
                   "%s: FLAC__stream_decoder_process_single "
                   "failed",
                   __func__);

          // TODO: should insert some abort condition?
          vTaskDelay(pdMS_TO_TICKS(10));
        }
      }

      // alternating chunk sizes need time stamp repair
      if ((cachedBlocks > 0) && (scSet->sr != 0)) {
        uint64_t diffUs = 1000000ULL * cachedBlocks / scSet->sr;

        uint64_t timestamp =
            1000000ULL * wire_chnk->timestamp.sec + wire_chnk->timestamp.usec;

        timestamp = timestamp - diffUs;

        wire_chnk->timestamp.sec = timestamp / 1000000ULL;
        wire_chnk->timestamp.usec = timestamp % 1000000ULL;
      }

      pcm_chunk_message_t* new_pcmChunk = NULL;
      int32_t ret = allocate_pcm_chunk_memory(&new_pcmChunk, pcmChunk.bytes);
      //      int32_t ret = -1;

      scSet->chkInFrames = FLAC__stream_decoder_get_blocksize(flacDecoder);

      // ESP_LOGE (TAG, "block size: %ld",
      // scSet->chkInFrames * scSet->bits / 8 * scSet->ch);
      // ESP_LOGI(TAG, "new_pcmChunk with size %ld",
      // new_pcmChunk->totalSize);

      if (ret == 0) {
        pcm_chunk_fragment_t* fragment = new_pcmChunk->fragment;
        uint32_t fragmentCnt = 0;

        if (fragment->payload != NULL) {
          uint32_t frames = pcmChunk.bytes / (scSet->ch * (scSet->bits / 8));

          for (int i = 0; i < frames; i++) {
            // TODO: for now fragmented payload is not
            // supported and the whole chunk is expected
            // to be in the first fragment
            uint32_t tmpData;
            memcpy(&tmpData, &pcmChunk.outData[fragmentCnt], sizeof(uint32_t));

            if (fragment != NULL) {
              volatile uint32_t* test =
                  (volatile uint32_t*)(&(fragment->payload[fragmentCnt]));
              *test = (volatile uint32_t)tmpData;
            }

            fragmentCnt += sizeof(uint32_t);
            // if (fragmentCnt >= fragment->size) {
            //   fragmentCnt = 0;

            //   fragment = fragment->nextFragment;
            // }
          }
        }

        new_pcmChunk->timestamp = wire_chnk->timestamp;

#if CONFIG_USE_DSP_PROCESSOR
        if (new_pcmChunk->fragment->payload) {
          dsp_processor_worker((void*)new_pcmChunk, (void*)scSet);
        }

#endif

        insert_pcm_chunk(new_pcmChunk);
        //        free_pcm_chunk(new_pcmChunk);
        //        new_pcmChunk = NULL;
      } else {
        ESP_LOGE(TAG, "failed to allocate chunk");
      }

      free(pcmChunk.outData);
      pcmChunk.outData = NULL;
      pcmChunk.bytes = 0;

      if (player_send_snapcast_setting(scSet) != pdPASS) {
        ESP_LOGE(TAG,
                 "Failed to "
                 "notify "
                 "sync task "
                 "about "
                 "codec. Did you "
                 "init player?");

        esp_restart();
      }

      break;
    }

    case PCM: {
      size_t decodedSize = wire_chnk->size;

      // ESP_LOGW(TAG, "got PCM chunk,"
      //               "typedMsgCurrentPos %d",
      //               parser.typedMsgCurrentPos);

      if (*pcmData) {
        (*pcmData)->timestamp = wire_chnk->timestamp;
      }

      scSet->chkInFrames =
          decodedSize / ((size_t)scSet->ch * (size_t)(scSet->bits / 8));

      // ESP_LOGW(TAG,
      //          "got PCM decoded chunk size: %ld
      //          frames", scSet->chkInFrames);

      if (player_send_snapcast_setting(scSet) != pdPASS) {
        ESP_LOGE(TAG,
                 "Failed to notify "
                 "sync task about "
                 "codec. Did you "
                 "init player?");

        esp_restart();
      }

#if CONFIG_USE_DSP_PROCESSOR
      if ((*pcmData) && ((*pcmData)->fragment->payload)) {
        dsp_processor_worker((void*)(*pcmData), (void*)scSet);
      }
#endif
      if (*pcmData) {
        insert_pcm_chunk(*pcmData);
      }

      *pcmData = NULL;
      free(decoderChunk.inData);
      decoderChunk.inData = NULL;

      break;
    }

    default: {
      ESP_LOGE(TAG,
               "Decoder (2) not "
               "supported");

      esp_restart();

      break;
    }
  }
}

/*
 * returns:
 * 0 if a message was (partially) processed sucessfully
 * -1 if network needs restart
 */
int process_data(snapcast_protocol_parser_t* parser,
                 time_sync_data_t* time_sync_data, bool* received_codec_header,
                 codec_type_t* codec, snapcastSetting_t* scSet,
                 pcm_chunk_message_t** pcmData, bool paused) {
  base_message_t base_message_rx;

  if (parse_base_message(parser, &base_message_rx) != PARSER_OK) {
    return -1;  // restart connection
  }
  time_sync_data->now = esp_timer_get_time();
  base_message_rx.received.sec = time_sync_data->now / 1000000;
  base_message_rx.received.usec =
      time_sync_data->now - base_message_rx.received.sec * 1000000;

  switch (base_message_rx.type) {
    case SNAPCAST_MESSAGE_WIRE_CHUNK: {
      wire_chunk_message_t wire_chnk = {
          {0, 0}, 0, NULL};  // is wire_chnk.payload ever used?

      // skip this wires chunk message if codec header message was not received
      // yet!
      if (*received_codec_header == false || paused) {
        if (parser_skip_typed_message(parser, &base_message_rx) != PARSER_OK) {
          return -1;  // restart connection
        }
        return 0;
      }

      if (parse_wire_chunk_message(parser, &base_message_rx, *codec, pcmData,
                                   &wire_chnk, &decoderChunk) != PARSER_OK) {
        return -1;
      }
      handle_chunk_message(*codec, scSet, pcmData, &wire_chnk);
      return 0;
    }

    case SNAPCAST_MESSAGE_CODEC_HEADER: {
      char* codecPayload = NULL;
      uint32_t codecPayloadLen = 0;
      int return_value = 0;
      if (parse_codec_header_message(parser, received_codec_header, codec,
                                     &codecPayload,
                                     &codecPayloadLen) != PARSER_OK) {
        return_value = -1;
      } else {
        codec_header_received(codecPayload, codecPayloadLen, *codec, scSet,
                              time_sync_data);
      }

      // in all cases: free Payload
      if (codecPayload != NULL) {
        free(codecPayload);
      }

      return return_value;
    }

    case SNAPCAST_MESSAGE_SERVER_SETTINGS: {
      server_settings_message_t server_settings_message;
      if (parse_sever_settings_message(parser, &base_message_rx,
                                       &server_settings_message) != PARSER_OK) {
        return -1;
      }
      server_settings_msg_received(&server_settings_message, scSet);
      return 0;
    }

    case SNAPCAST_MESSAGE_TIME: {
      time_message_t time_message_rx;
      if (parse_time_message(parser, &base_message_rx, &time_message_rx) !=
          PARSER_OK) {
        return -1;
      }
      time_sync_msg_received(&base_message_rx, &time_message_rx, time_sync_data,
                             *received_codec_header);
      return 0;
    }

    default: {
      if (parser_skip_typed_message(parser, &base_message_rx) != PARSER_OK) {
        return -1;
      }

      ESP_LOGD(TAG, "done skipping typed message %d", base_message_rx.type);
      return 0;
    }
  }

  return 0;
}

typedef struct {
  time_sync_data_t* time_sync_data;
  bool* received_codec_header;
} before_receive_callback_data_t;

void before_receive_callback(before_receive_callback_data_t* data) {
  // unpack
  time_sync_data_t* time_sync_data = data->time_sync_data;
  bool received_codec_header = *data->received_codec_header;

  time_sync_data->now = esp_timer_get_time();
  // send time sync message
  if ((received_codec_header &&
       (time_sync_data->now - time_sync_data->lastTimeSyncSent) >=
           time_sync_data->timeout)) {
    time_sync_msg_cb(NULL);
    time_sync_data->lastTimeSyncSent = time_sync_data->now;

    // ESP_LOGI(TAG, "time sync sent after %lluus", timeout);
  }
}

void http_player_state_changed() {
  xTaskNotifyGive(t_http_get_task);
  // ESP_LOGI(TAG, "http task cb");
}

/**
 *
 */
static void http_get_task(void* pvParameters) {
  connection_t connection;
  connection.firstNetBuf = NULL;
  connection.rc1 = ERR_OK;
  connection.netif = NULL;
  int rc1;  // for local scope (handshake), independent of connection.rc1
  base_message_t base_message_rx;
  hello_message_t hello_message;
  char* hello_message_serialized = NULL;
  static char device_hostname[64] = {0};  // Buffer for hostname
  int result;
  time_sync_data_t time_sync_data;
  time_sync_data.lastTimeSync = 0;
  time_sync_data.lastTimeSyncSent = 0;
  bool received_codec_header = false;
  codec_type_t codec = NONE;
  snapcastSetting_t scSet;
  pcm_chunk_message_t* pcmData = NULL;
  bool paused = false;

  // create a timer to send time sync messages every x µs
  //  esp_timer_create(&tSyncArgs, &time_sync_data.timeSyncMessageTimer);

  idCounterSemaphoreHandle = xSemaphoreCreateMutex();
  if (idCounterSemaphoreHandle == NULL) {
    ESP_LOGE(TAG, "can't create id Counter Semaphore");

    esp_restart();
  }

  add_player_state_cb(http_player_state_changed);

  while (1) {
    // do some house keeping
    {
      //      esp_timer_stop(time_sync_data.timeSyncMessageTimer);

      received_codec_header = false;

      xSemaphoreTake(idCounterSemaphoreHandle, portMAX_DELAY);
      id_counter = 0;
      xSemaphoreGive(idCounterSemaphoreHandle);

      if (opusDecoder != NULL) {
        opus_decoder_destroy(opusDecoder);
        opusDecoder = NULL;
      }

      if (flacDecoder != NULL) {
        FLAC__stream_decoder_finish(flacDecoder);
        FLAC__stream_decoder_delete(flacDecoder);
        flacDecoder = NULL;
      }

      if (decoderChunk.inData) {
        free(decoderChunk.inData);
        decoderChunk.inData = NULL;
      }

      if (decoderChunk.outData) {
        free(decoderChunk.outData);
        decoderChunk.outData = NULL;
      }
    }

    // NETWORK setup ends here ( or before getting mac address )
    setup_network(&connection.netif);

    // if (reset_latency_buffer() < 0) {
    //   ESP_LOGE(TAG,
    //            "reset_diff_buffer: couldn't reset median filter long. STOP");
    //   return;
    // }

    uint8_t base_mac[6];
#if CONFIG_SNAPCLIENT_USE_INTERNAL_ETHERNET || \
    CONFIG_SNAPCLIENT_USE_SPI_ETHERNET
    // Get MAC address for Eth Interface
    char eth_mac_address[18];

    esp_read_mac(base_mac, ESP_MAC_ETH);
    sprintf(eth_mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", base_mac[0],
            base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);
    ESP_LOGI(TAG, "eth mac: %s", eth_mac_address);
#endif
    // Get MAC address for WiFi station
    char mac_address[18];
    esp_read_mac(base_mac, ESP_MAC_WIFI_STA);
    sprintf(mac_address, "%02X:%02X:%02X:%02X:%02X:%02X", base_mac[0],
            base_mac[1], base_mac[2], base_mac[3], base_mac[4], base_mac[5]);
    ESP_LOGI(TAG, "sta mac: %s", mac_address);

    time_sync_data.now = esp_timer_get_time();

    // init base message
    base_message_rx.type = SNAPCAST_MESSAGE_HELLO;
    xSemaphoreTake(idCounterSemaphoreHandle, portMAX_DELAY);
    base_message_rx.id = id_counter++;
    xSemaphoreGive(idCounterSemaphoreHandle);

    base_message_rx.refersTo = 0x0000;
    base_message_rx.sent.sec = time_sync_data.now / 1000000;
    base_message_rx.sent.usec =
        time_sync_data.now - base_message_rx.sent.sec * 1000000;
    base_message_rx.received.sec = 0;
    base_message_rx.received.usec = 0;
    base_message_rx.size = 0x00000000;

    // init hello message
    hello_message.mac = mac_address;

    // Get hostname from NVS or fallback to a sensible default
    if (settings_get_hostname(device_hostname, sizeof(device_hostname)) !=
        ESP_OK) {
      strncpy(device_hostname, "snapclient", sizeof(device_hostname) - 1);
    }
    hello_message.hostname = device_hostname;

    hello_message.version = (char*)VERSION_STRING;
    hello_message.client_name = "libsnapcast";
    hello_message.os = "esp32";
    hello_message.arch = "xtensa";
    hello_message.instance = 1;
    hello_message.id = mac_address;
    hello_message.protocol_version = 2;

    if (hello_message_serialized == NULL) {
      hello_message_serialized = hello_message_serialize(
          &hello_message, (size_t*)&(base_message_rx.size));
      if (!hello_message_serialized) {
        ESP_LOGE(TAG, "Failed to serialize hello message");
        esp_restart();
      }
    }

    result = base_message_serialize(&base_message_rx, base_message_serialized,
                                    BASE_MESSAGE_SIZE);
    if (result) {
      ESP_LOGE(TAG, "Failed to serialize base message");
      esp_restart();
    }

    rc1 = netconn_write(lwipNetconn, base_message_serialized, BASE_MESSAGE_SIZE,
                        NETCONN_NOCOPY);
    rc1 |= netconn_write(lwipNetconn, hello_message_serialized,
                         base_message_rx.size, NETCONN_NOCOPY);
    if (rc1 != ERR_OK) {
      ESP_LOGE(TAG, "netconn failed to send hello message");

      continue;
    }

    ESP_LOGI(TAG, "netconn sent hello message");

    free(hello_message_serialized);
    hello_message_serialized = NULL;

    // init default setting
    scSet.buf_ms = 500;
    scSet.codec = NONE;
    scSet.bits = 16;
    scSet.ch = 2;
    scSet.sr = 44100;
    scSet.chkInFrames = 0;
    scSet.muted = true;

    snapcast_protocol_parser_t parser;

    // state machine starts here

    before_receive_callback_data_t before_receive_callback_data;
    before_receive_callback_data.received_codec_header = &received_codec_header;
    before_receive_callback_data.time_sync_data = &time_sync_data;
    connection.before_receive_callback =
        (void (*)(void*))before_receive_callback;
    connection.before_receive_callback_data =
        (void*)&before_receive_callback_data;
    connection.isMuted = &scSet.muted;

    connection.firstNetBuf = NULL;
    connection.first_receive = true;
    connection.first_netbuf_processed = false;

    connection.state = CONNECTION_INITIALIZED;

    parser.get_byte_context = &connection;
    parser.get_byte_function = (get_byte_callback_t)(&connection_get_byte);

    // as we need fast time syncs in the beginning we set receive timeout very
    // low
    time_sync_data.timeout = FAST_SYNC_LATENCY_BUF;
    netconn_set_recvtimeout(lwipNetconn,
                            time_sync_data.timeout / 1000);  // timeout in ms

    // Main connection loop - state machine + data processing
    while (1) {
      if (ulTaskNotifyTake(pdTRUE, 1) == pdTRUE) {
        // state change, e.g. pause/play
        paused = get_player_state() == PAUSED;
        // ESP_LOGI(TAG, "http got cb. %s", paused ? "paused" : "playing/idle");
      }
      int result =
          process_data(&parser, &time_sync_data, &received_codec_header, &codec,
                       &scSet, &pcmData, paused);
      if (result != 0) {
        break;  // restart connection
      }
    }
  }
}

/**
 *
 */
int init_snapcast(void (*set_volume)(int), void (*set_mute)(bool)) {
  if (set_volume == NULL) {
    ESP_LOGE(TAG, "Volume callback is NULL");

    return -1;
  }
  if (set_mute == NULL) {
    ESP_LOGE(TAG, "Mute callback is NULL");

    return -1;
  }
  set_volume_cb = set_volume;
  set_mute_cb = set_mute;
  return 0;
}

void start_snapcast() {
#if CONFIG_FREERTOS_NUMBER_OF_CORES == 1
  // Single-core targets (S2, C3/C6/H2, ...) only have core 0. Pinning to
  // HTTP_TASK_CORE_ID (1) trips the xCoreID < configNUMBER_OF_CORES assert
  // in FreeRTOS and boot-loops the chip, so let the scheduler place the task.
  xTaskCreate(&http_get_task, "http", 15 * 1024, NULL,
              HTTP_TASK_PRIORITY, &t_http_get_task);
#else
  xTaskCreatePinnedToCore(&http_get_task, "http", 15 * 1024, NULL,
                          HTTP_TASK_PRIORITY, &t_http_get_task,
                          HTTP_TASK_CORE_ID);
#endif
  ESP_LOGD(TAG, "Started snapcast client task");
}
