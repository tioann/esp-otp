// Management protocol over the C3's native USB-Serial/JTAG.
//
// Reads framed protocol requests, dispatches them (physical USB access counts
// as authorized) and writes framed responses. Frames are SOF+CRC delimited so
// they coexist with interleaved log output on the same channel.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Install the USB-Serial/JTAG driver and start the reader task.
esp_err_t usbcon_init(void);

#ifdef __cplusplus
}
#endif
