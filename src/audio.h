#pragma once

//================ I2S =================
#define I2S_BCLK 21
#define I2S_WS   45
#define I2S_DOUT 47

//================ SD ==================
#define SD_SCK   12
#define SD_MISO  13
#define SD_MOSI  11
#define SD_CS    10

void audioInit();
void playMusic();
void stopMusic();
void audioUpdate();
