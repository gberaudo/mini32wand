/**
 * This project is a magical wand based on ESP32.
 * The hardware is made of:
 * - a wemos compatible D1 mini 32;
 * - a strip piece of 10 WS2812B LEDs;
 * - a Li-ion 18650C battery + USB cable cut and plugged on it
 * - some cardboard and glue
 * - some aluminium foil to create a capacitive button
*/
#include <Arduino.h>
#include <FastLED.h>


FASTLED_USING_NAMESPACE

#define LED_PIN  D4
#define TOUCH_PIN T3

#define COLOR_ORDER GRB
#define CHIPSET     WS2812B

#define BRIGHTNESS 255

#define NUM_LEDS 10


// Define the array of leds
CRGB leds[NUM_LEDS];

void setup() {
    Serial.begin(115200);
    delay(300);  // limit the peak of current when starting
    FastLED.setMaxPowerInVoltsAndMilliamps(5, 200);  // limit the current drawn by the LEDs
    FastLED.setBrightness(BRIGHTNESS);
    FastLED.addLeds<NEOPIXEL, LED_PIN>(leds, NUM_LEDS);  // GRB ordering is assumed
    Serial.println("Magic wand started");
}


uint8_t gHue = 0; // the base color used by the effects

unsigned long touching_since = 0;
unsigned long button_released_after = 0;

inline void handleTouching() {
  touch_value_t touchValue = touchRead(TOUCH_PIN);
  if (touchValue < 50) {
    // button pressed
    if (!touching_since) {
      touching_since = millis();
    }
  } else {
    // button released
    if (touching_since) {
      button_released_after = millis() - touching_since;
      touching_since = 0;
    }
  }
}


/**
 * A nice blast effect in 3 parts
*/
void blast(unsigned long duration, uint8_t fadeBy) {
  // light progressively to the tip
  for (uint8_t i = 0; i < (NUM_LEDS / 2); i++) {
    fadeToBlackBy(leds, NUM_LEDS, fadeBy);
    uint8_t brightness = (uint8_t)(255 * i / (NUM_LEDS / 2));
    leds[NUM_LEDS -1 - i] = CHSV(gHue, 255, 125 + brightness / 2);
    leds[i] = CHSV(gHue, 255, 125 + brightness / 2);
    FastLED.show();
    FastLED.delay(duration);
  }

  // have a white flash at the tip of the wand
  uint8_t tip_start = ceil(NUM_LEDS / 4.0);
  for (uint8_t i = 0; i < 9; i++) {
    fadeToBlackBy(leds, NUM_LEDS, fadeBy);
    if (i % 2) {
      fill_solid(leds + 3, 4, CRGB::White);
    } else {
      fill_solid(leds + 3, 4, CHSV(gHue, 255, 255));
    }
    FastLED.show();
    FastLED.delay(duration / 3);
  }

  // glitter around the tip
  for (uint8_t i = 0; i < 40; i++) {
     if( random8() < 160) {
      size_t pos = 1 + random8(8);
      leds[pos] = CHSV(gHue, 255, 80);
      FastLED.show();
      leds[pos] = CRGB::Black;
    }
    FastLED.delay(duration / 3);
  }
  fill_solid(leds, NUM_LEDS, CRGB::Black);
}


// Glittering effect and hue selection while pressing
unsigned long glittering_time = 0;
void handle_glittering() {
  if (millis() < glittering_time) {
    if( random8() < 80) {
      size_t pos = random16(NUM_LEDS) ;
      leds[pos] = CHSV(gHue, 255, 80 + random(160));
      FastLED.show();
      leds[pos] = CRGB::Black;
    }
  }
}


void loop()
{
  handleTouching();
  if (button_released_after > 500) {
    Serial.println("start a blast");
    Serial.println(button_released_after);
    button_released_after = 0;
    blast(40, 150);  // blasting is blocking.
  }

  if (touching_since) {
    glittering_time = millis() + 200;
  }

  handle_glittering();

  FastLED.show();
  EVERY_N_MILLISECONDS( 15 ) { gHue++; }
}
