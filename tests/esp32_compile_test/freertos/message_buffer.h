/*
 * Stub freertos/message_buffer.h for ESP32 HAL compilation test.
 */
#ifndef FREERTOS_MESSAGE_BUFFER_H
#define FREERTOS_MESSAGE_BUFFER_H

#include "freertos/FreeRTOS.h"
#include <string.h>

MessageBufferHandle_t xMessageBufferCreate(size_t bufferSizeBytes);
void vMessageBufferDelete(MessageBufferHandle_t msgbuf);
size_t xMessageBufferSend(MessageBufferHandle_t msgbuf, const void *data, size_t len, TickType_t ticks);
size_t xMessageBufferReceive(MessageBufferHandle_t msgbuf, void *buf, size_t buf_len, TickType_t ticks);
size_t xMessageBufferSpacesAvailable(MessageBufferHandle_t msgbuf);

#endif /* FREERTOS_MESSAGE_BUFFER_H */
