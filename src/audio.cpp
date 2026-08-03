#include "globals.h"
#include "audio.h"
#include "AudioTools.h"
#include "AudioTools/Disk/AudioSourceSD.h"
#include "AudioTools/AudioCodecs/CodecMP3Helix.h"

uint32_t fadeInTime = 10000;
uint32_t fadeOutTime = 5000;
bool isPlaying = false;
bool sdOK = false;

static bool isFadeIn = false;
static bool isFadeOut = false;
static uint32_t fadeStart = 0;

static const float MAX_VOLUME = 1.0f;

SPIClass spiSD(FSPI);

// Chỉ để 1 file mp3 trong thẻ
static AudioSourceSD source("/", "mp3", SD_CS, spiSD);

static I2SStream i2s;
static MP3DecoderHelix decoder;
static AudioPlayer player(source, i2s, decoder);

void audioInit()
{
  fadeInTime = prefs.getUInt("fadein", 10000);
  fadeOutTime = prefs.getUInt("fadeout", 5000);

  spiSD.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdOK = SD.begin(SD_CS, spiSD);

  if (sdOK)
      Serial.println("SD OK");
  else
      Serial.println("SD Init Failed!");

  auto cfg = i2s.defaultConfig(TX_MODE);
  cfg.pin_bck = I2S_BCLK;
  cfg.pin_ws = I2S_WS;
  cfg.pin_data = I2S_DOUT;
  i2s.begin(cfg);
  player.setVolume(0);
}

//========================================================
// PLAY
//========================================================
void playMusic() {

  if (!sdOK) {
      Serial.println("No SD card");
      return;
  }

  // Nếu đang fade out thì hủy fade out và fade in tiếp
  if (isFadeOut) {

    isFadeOut = false;
    isFadeIn = true;
    fadeStart = millis();

    Serial.println("Resume Fade In");
    return;
  }

  // Đang phát bình thường thì thôi
  if (isPlaying)
    return;

  player.begin();

  player.setVolume(0);

  fadeStart = millis();

  isPlaying = true;
  isFadeIn = true;
  isFadeOut = false;

  Serial.println("PLAY");
}

//========================================================
// STOP (Fade Out)
//========================================================
void stopMusic() {

  if (!isPlaying)
    return;

  if (isFadeOut)
    return;

  isFadeIn = false;
  isFadeOut = true;
  fadeStart = millis();

  Serial.println("STOP");
}

// Called every loop(): drives the fade envelope and services the decoder. Mirrors the tail
// of the old monolithic loop() 1:1, just extracted into its own subsystem function.
void audioUpdate() {

  if (!isPlaying)
    return;

  if (!player.copy()) {
    Serial.println("Song Finished");
    player.end();
    isPlaying = false;
    isFadeIn = false;
    isFadeOut = false;
    return;
  }

  uint32_t now = millis();

  //================ Fade In =================

  if (isFadeIn) {
    float p = (float)(now - fadeStart) / fadeInTime;
    if (p >= 1.0f) {
      p = 1.0f;
      isFadeIn = false;
    }
    player.setVolume(MAX_VOLUME * p);
  }

  //================ Fade Out =================

  if (isFadeOut) {
    float p = (float)(now - fadeStart) / fadeOutTime;
    if (p >= 1.0f) {
      player.setVolume(0);
      player.end();
      isPlaying = false;
      isFadeOut = false;
      Serial.println("Stopped");
    } else {
      player.setVolume(MAX_VOLUME * (1.0f - p));
    }
  }
}
