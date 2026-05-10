/*
 * Stub unistd.h for ESP32 HAL compilation test.
 *
 * Provides only the POSIX I/O functions needed by the HAL.
 * Avoids including <io.h> which declares open()/close() as functions
 * that conflict with ESP-IDF's union member names 'open' and 'close'.
 */
#ifndef UNISTD_H
#define UNISTD_H

#include <sys/types.h>

ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int close(int fd);

#endif /* UNISTD_H */
