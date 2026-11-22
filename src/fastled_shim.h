/**
 * fastled_shim.h - minimal FastLED-compatible API for the nRF54 Arduino core.
 *
 * FastLED (as of 3.10.x) has no nRF54 platform support: its Nordic code path
 * is gated on NRF52_SERIES and expects nRF52 headers/peripherals, which this
 * core (ARDUINO_ARCH_NRF54L15CLEAN) does not provide. This shim implements
 * the small subset of the FastLED API used by this project on top of:
 *   - a cycle-accurate WS2812B bit-bang driver (SysTick-based timing), and
 *   - color math ported verbatim from FastLED 3.10.8 (hsv2rgb_rainbow,
 *     scale8, ...) so hues and fades match the original ESP32 build.
 *
 * Only compiled for the nRF54 core; the ESP32 environment keeps using the
 * real FastLED library.
 */
#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------- colors

struct CRGB;

struct CHSV {
    uint8_t hue;  // 0-255 color wheel position
    uint8_t sat;  // 0-255 saturation
    uint8_t val;  // 0-255 brightness
    constexpr CHSV(uint8_t h, uint8_t s, uint8_t v) : hue(h), sat(s), val(v) {}
};

// Declared before CRGB so its implicit CHSV conversion can call it.
CRGB hsv2rgb_rainbow(const CHSV& hsv);

struct CRGB {
    uint8_t r;
    uint8_t g;
    uint8_t b;

    enum HTMLColorCode : uint32_t {
        Black = 0x000000,
        White = 0xFFFFFF,
        Red = 0xFF0000,
        Green = 0x008000,
        Blue = 0x0000FF,
        Yellow = 0xFFFF00,
        Cyan = 0x00FFFF,
        Magenta = 0xFF00FF,
        Orange = 0xFFA500,
    };

    constexpr CRGB() : r(0), g(0), b(0) {}
    constexpr CRGB(uint8_t red, uint8_t green, uint8_t blue)
        : r(red), g(green), b(blue) {}
    constexpr CRGB(HTMLColorCode code)
        : r(uint8_t((code >> 16) & 0xFF)), g(uint8_t((code >> 8) & 0xFF)),
          b(uint8_t(code & 0xFF)) {}
    // Implicit CHSV conversion, like FastLED (rainbow palette).
    CRGB(const CHSV& hsv) { *this = hsv2rgb_rainbow(hsv); }
    CRGB& operator=(const CHSV& hsv) {
        *this = hsv2rgb_rainbow(hsv);
        return *this;
    }
};

// ------------------------------------------------- lib8tion math (FastLED)
// Ported from FastLED 3.10.8 (MIT license), FASTLED_SCALE8_FIXED == 1.

static inline uint8_t scale8(uint8_t i, uint8_t scale) {
    return (((uint16_t)i) * (1 + (uint16_t)(scale))) >> 8;
}

static inline uint8_t scale8_video(uint8_t i, uint8_t scale) {
    return (((int)i * (int)scale) >> 8) + ((i && scale) ? 1 : 0);
}

// ------------------------------------------------------------ color utils

void fadeToBlackBy(CRGB* leds, uint16_t numLeds, uint8_t fadeBy);

void fill_solid(CRGB* leds, int numLeds, const CRGB& color);

// -------------------------------------------------------------- PRNG
// FastLED's 16-bit LCG (lib8tion/random8.h).

uint8_t random8();
uint8_t random8(uint8_t lim);
uint16_t random16();
uint16_t random16(uint16_t lim);

// ----------------------------------------------------------- macros

// Same expansion shape as FastLED's EVERY_N_MILLIS_I: declare a timer object
// and leave `if (timer)` for the caller's `{ ... }`.
class CEveryNMillis {
  public:
    explicit CEveryNMillis(uint32_t period) : mPeriod(period), mLastTrigger(0) {}
    operator bool() {
        const uint32_t now = millis();
        if (now - mLastTrigger >= mPeriod) {
            mLastTrigger = now;
            return true;
        }
        return false;
    }

  private:
    uint32_t mPeriod;
    uint32_t mLastTrigger;
};

#define FASTLED_SHIM_CONCAT_(a, b) a##b
#define FASTLED_SHIM_CONCAT(a, b) FASTLED_SHIM_CONCAT_(a, b)
#define EVERY_N_MILLISECONDS(N)                                                \
    static CEveryNMillis FASTLED_SHIM_CONCAT(s_everyNms_, __LINE__)((N));      \
    if (FASTLED_SHIM_CONCAT(s_everyNms_, __LINE__))

// ------------------------------------------------------ WS2812B driver

// Maps the Arduino pin to its GPIO port registers and sets it as output,
// idle low. Must be called before the first frame (done by addLeds).
void ws2812_init(uint8_t pin);

// --------------------------------------------------------- LED controller

// Only the chipset constant used by this project; accepted but ignored
// (the driver is always the WS2812B/NEOPIXEL bit-bang one).
enum ESPIChipset { NEOPIXEL };

class CFastLED {
  public:
    template <uint8_t CHIPSET, uint8_t DATA_PIN>
    void addLeds(CRGB* data, int nLeds) {
        (void)CHIPSET;
        mLeds = data;
        mCount = (uint16_t)nLeds;
        ws2812_init(DATA_PIN);
    }

    // Shows the current frame; applies the power limit (if set) the same way
    // FastLED does: by capping the brightness scale before rendering.
    void show();

    // FastLED semantics: keeps re-showing the frame while waiting.
    void delay(unsigned long ms);

    void setBrightness(uint8_t scale) { mScale = scale; }
    uint8_t getBrightness() const { return mScale; }

    void setMaxPowerInVoltsAndMilliamps(uint8_t volts, uint32_t milliamps) {
        mPowerLimitmW = (uint32_t)volts * milliamps;
    }

  private:
    CRGB* mLeds = nullptr;
    uint16_t mCount = 0;
    uint8_t mScale = 255;
    uint32_t mPowerLimitmW = 0;
};

extern CFastLED FastLED;
