#ifndef TFT_DISPLAY_H
#define TFT_DISPLAY_H

#include <Arduino.h>
#include "types.h"

// TFT_eSPI must be included outside #ifdef for PlatformIO's LDF to detect it
// Configuration is done via build flags in platformio.ini
#include <TFT_eSPI.h>

#ifdef LCD_ENABLED

// Initialize display hardware and create display task
void initDisplay();

// Start the display task on Core 0
void startDisplayTask();

// Display task function (runs on Core 0)
void displayTask(void* parameter);

#endif // LCD_ENABLED

#endif // TFT_DISPLAY_H
