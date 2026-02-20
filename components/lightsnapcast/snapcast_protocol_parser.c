#include <string.h>
#include "snapcast_protocol_parser.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

// MACROS for reading from connection
#define READ_BYTE(parser, dest) \
  do { \
    char _byte; \
    if ((parser)->get_byte_function((parser)->get_byte_context, &_byte) != 0) { \
      return PARSER_CONNECTION_ERROR; \
    } \
    (dest) = _byte; \
  } while(0)

#define READ_UINT16_LE(parser, dest) \
  do { \
    char _bytes[2]; \
    if ((parser)->get_byte_function((parser)->get_byte_context, &_bytes[0]) != 0 || \
        (parser)->get_byte_function((parser)->get_byte_context, &_bytes[1]) != 0) { \
      return PARSER_CONNECTION_ERROR; \
    } \
    (dest) = (_bytes[0] & 0xFF) | ((_bytes[1] & 0xFF) << 8); \
  } while(0)

#define READ_UINT32_LE(parser, dest) \
  do { \
    char _bytes[4]; \
    for (int _i = 0; _i < 4; _i++) { \
      if ((parser)->get_byte_function((parser)->get_byte_context, &_bytes[_i]) != 0) { \
        return PARSER_CONNECTION_ERROR; \
      } \
    } \
    (dest) = (_bytes[0] & 0xFF) | ((_bytes[1] & 0xFF) << 8) | \
             ((_bytes[2] & 0xFF) << 16) | ((_bytes[3] & 0xFF) << 24); \
  } while(0)

#define READ_TIMESTAMP(parser, ts) \
  do { \
    READ_UINT32_LE(parser, (ts).sec); \
    READ_UINT32_LE(parser, (ts).usec); \
  } while(0)

#define READ_DATA(parser, dest, len) \
  do { \
    for (uint32_t _i = 0; _i < (len); _i++) { \
      if ((parser)->get_byte_function((parser)->get_byte_context, &(dest)[_i]) != 0) { \
        return PARSER_CONNECTION_ERROR; \
      } \
    } \
  } while(0)

#define READ_DATA_WITH_CLEANUP(parser, dest, len, cleanup) \
  do { \
    for (uint32_t _i = 0; _i < (len); _i++) { \
      if ((parser)->get_byte_function((parser)->get_byte_context, &(dest)[_i]) != 0) { \
        cleanup; \
        return PARSER_CONNECTION_ERROR; \
      } \
    } \
  } while(0)

static const char* TAG = "SNAPCAST_PROTOCOL_PARSER";

parser_return_state_t parse_base_message(snapcast_protocol_parser_t* parser,
                                         base_message_t* base_message_rx) {
  READ_UINT16_LE(parser, base_message_rx->type);
  READ_UINT16_LE(parser, base_message_rx->id);
  READ_UINT16_LE(parser, base_message_rx->refersTo);
  READ_TIMESTAMP(parser, base_message_rx->sent);
  READ_TIMESTAMP(parser, base_message_rx->received);
  READ_UINT32_LE(parser, base_message_rx->size);

  return PARSER_COMPLETE;
}



parser_return_state_t parse_wire_chunk_message(snapcast_protocol_parser_t* parser,
                                               base_message_t* base_message_rx,
                                               bool received_codec_header,
                                               codec_type_t codec,
                                               pcm_chunk_message_t** pcmData,
                                               wire_chunk_message_t* wire_chnk,
                                               decoderData_t* decoderChunk) {

  READ_TIMESTAMP(parser, wire_chnk->timestamp);
  READ_UINT32_LE(parser, wire_chnk->size);

  // TODO: we could use wire chunk directly maybe?
  decoderChunk->bytes = wire_chnk->size;
  while (!decoderChunk->inData) {
    decoderChunk->inData = (uint8_t*)malloc(decoderChunk->bytes);
    if (!decoderChunk->inData) {
      ESP_LOGW(TAG,
               "malloc decoderChunk->inData failed, wait "
               "1ms and try again");

      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }

  uint32_t payloadOffset = 0;
  uint32_t tmpData = 0;
  int32_t payloadDataShift = 0;
  size_t tmp_size = base_message_rx->size - 12;

  if (received_codec_header == true) {
    switch (codec) {
      case OPUS:
      case FLAC: {
        READ_DATA(parser, (char*)decoderChunk->inData, tmp_size);
        payloadOffset += tmp_size;
        decoderChunk->outData = NULL;
        decoderChunk->type = SNAPCAST_MESSAGE_WIRE_CHUNK;

        break;
      }

      case PCM: {
        size_t _tmp = tmp_size;

        if (*pcmData == NULL) {
          if (allocate_pcm_chunk_memory(pcmData, wire_chnk->size) < 0) {
            *pcmData = NULL;
          }

          tmpData = 0;
          payloadDataShift = 3;
          payloadOffset = 0;
        }

        while (_tmp--) {
          char tmp_val;
          READ_BYTE(parser, tmp_val);
          tmpData |= ((uint32_t)tmp_val << (8 * payloadDataShift));

          payloadDataShift--;
          if (payloadDataShift < 0) {
            payloadDataShift = 3;

            if ((*pcmData) && ((*pcmData)->fragment->payload)) {
              volatile uint32_t* sample;
              uint8_t dummy1;
              uint32_t dummy2 = 0;

              // TODO: find a more
              // clever way to do this,
              // best would be to
              // actually store it the
              // right way in the first
              // place
              dummy1 = tmpData >> 24;
              dummy2 |= (uint32_t)dummy1 << 16;
              dummy1 = tmpData >> 16;
              dummy2 |= (uint32_t)dummy1 << 24;
              dummy1 = tmpData >> 8;
              dummy2 |= (uint32_t)dummy1 << 0;
              dummy1 = tmpData >> 0;
              dummy2 |= (uint32_t)dummy1 << 8;
              tmpData = dummy2;

              sample = (volatile uint32_t *)(&((*pcmData)->fragment->payload[payloadOffset]));
              *sample = (volatile uint32_t)tmpData;

              payloadOffset += 4;
            }

            tmpData = 0;
          }
        }

        break;
      }
      default: {
        ESP_LOGE(TAG, "Decoder (1) not supported");
        return PARSER_CRITICAL_ERROR;
      }
    }
  }

  if (received_codec_header == true) {
    return PARSER_COMPLETE;
  } else {
    return PARSER_INCOMPLETE;  // TODO: right return value?
  }
}

parser_return_state_t parse_codec_header_message(
    snapcast_protocol_parser_t* parser,
    bool* received_codec_header, codec_type_t* codec,
    char** codecPayload, uint32_t* codecPayloadLen) {
  *received_codec_header = false;

  uint32_t codecStringLen = 0;
  READ_UINT32_LE(parser, codecStringLen);

  char codecString[8]; // longest supported string has 4 + 1 chars

  if (codecStringLen + 1 > sizeof(codecString)) {
    READ_DATA(parser, codecString, sizeof(codecString)-1);
    codecString[sizeof(codecString)-1] = 0; // null terminate
    ESP_LOGE(TAG, "Codec : %s... not supported (length: %lu)", codecString, codecStringLen);
    ESP_LOGI(TAG,
             "Change encoder codec to "
             "opus, flac or pcm in "
             "/etc/snapserver.conf on "
             "server");
    return PARSER_CRITICAL_ERROR;
  }
  READ_DATA(parser, codecString, codecStringLen);

  // NULL terminate string
  codecString[codecStringLen] = 0;

  // ESP_LOGI (TAG, "got codec string: %s", tmp);

  if (strcmp(codecString, "opus") == 0) {
    *codec = OPUS;
  } else if (strcmp(codecString, "flac") == 0) {
    *codec = FLAC;
  } else if (strcmp(codecString, "pcm") == 0) {
    *codec = PCM;
  } else {
    *codec = NONE;

    ESP_LOGI(TAG, "Codec : %s not supported", codecString);
    ESP_LOGI(TAG,
             "Change encoder codec to "
             "opus, flac or pcm in "
             "/etc/snapserver.conf on "
             "server");

    return PARSER_CRITICAL_ERROR;
  }

  //
  READ_UINT32_LE(parser, *codecPayloadLen);

  *codecPayload = malloc(*codecPayloadLen);  // allocate memory
                                             // for codec payload
  if (*codecPayload == NULL) {
    ESP_LOGE(TAG,
             "couldn't get memory "
             "for codec payload");

    return PARSER_CRITICAL_ERROR;
  }

  READ_DATA(parser, *codecPayload, *codecPayloadLen);

  *received_codec_header = true;

  return PARSER_COMPLETE;
}

parser_return_state_t parse_sever_settings_message(
    snapcast_protocol_parser_t* parser, base_message_t* base_message_rx,
    server_settings_message_t* server_settings_message) {
  uint32_t typedMsgLen;
  char* serverSettingsString = NULL;
  READ_UINT32_LE(parser, typedMsgLen);

  // ESP_LOGI(TAG,"server settings string is %lu long", typedMsgLen);

  // now get some memory for server settings string
  serverSettingsString = malloc(typedMsgLen + 1);
  if (serverSettingsString == NULL) {
    ESP_LOGE(TAG,
             "couldn't get memory for "
             "server settings string");
    return PARSER_CRITICAL_ERROR;
  }

  size_t tmpSize = base_message_rx->size - 4;
  // TODO: should there be an assert that tmpSize <= typedMsgLen?

  READ_DATA_WITH_CLEANUP(parser, serverSettingsString, tmpSize,
    free(serverSettingsString) // This will be executed on error before returning
  );

  serverSettingsString[typedMsgLen] = 0;

  // ESP_LOGI(TAG, "got string: %s",
  // serverSettingsString);

  int deserialization_result;

  deserialization_result = server_settings_message_deserialize(
      server_settings_message, serverSettingsString);

  free(serverSettingsString);

  if (deserialization_result) {
    ESP_LOGE(TAG,
             "Failed to read server "
             "settings: %d",
             deserialization_result);
    return PARSER_CRITICAL_ERROR;
  }

  return PARSER_COMPLETE;  // do callback

}


parser_return_state_t parse_time_message(snapcast_protocol_parser_t* parser,
                                         base_message_t* base_message_rx,
                                         time_message_t* time_message_rx) {
  READ_TIMESTAMP(parser, time_message_rx->latency);

  if (base_message_rx->size < 8) { // TODO: how to handle this case? Do we NEED to check?
    ESP_LOGE(TAG,
             "error time message, this shouldn't happen! %d %ld",
             8, base_message_rx->size);
    return PARSER_INCOMPLETE;  // use this return value as "ignore"
  }

  // ESP_LOGI(TAG, "done time message");
  return PARSER_COMPLETE;  // do callback
}

parser_return_state_t parse_unknown_message(snapcast_protocol_parser_t* parser,
                                            base_message_t* base_message_rx) {
  // For unknown messages, we need to consume all remaining bytes
  char dummy_byte;
  for (uint32_t i = 0; i < base_message_rx->size; i++) {
    READ_BYTE(parser, dummy_byte);
  }

  ESP_LOGI(TAG, "done unknown typed message %d", base_message_rx->type);

  return PARSER_COMPLETE;
}
