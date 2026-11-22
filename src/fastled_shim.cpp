/**
 * fastled_shim.cpp - WS2812B driver + FastLED API implementation for nRF54.
 *
 * See fastled_shim.h for the rationale. Compiled only for the nRF54 Arduino
 * core; on other architectures (e.g. the ESP32 environment) this file is an
 * empty translation unit and the real FastLED library is used instead.
 */
#include "fastled_shim.h"

#ifdef ARDUINO_ARCH_NRF54L15CLEAN

#include <stdint.h>

// Defined in the core's system_nrf54lm20b.c; not declared by cmsis.h.
extern "C" uint32_t SystemCoreClock;

// Chip register definitions (NRF_P0..NRF_P3, NRF_GPIO_Type). Arduino.h does
// not pull the chip header in, so include it directly.
#if defined(__has_include)
#if __has_include("nrf54lm20b.h")
#include "nrf54lm20b.h"
#elif __has_include("nrf54l15.h")
#include "nrf54l15.h"
#endif
#else
#include "nrf54lm20b.h"
#endif

// =========================================================== WS2812B driver
//
// The core runs the CPU (and therefore SysTick, which it configures as a
// processor-clock down-counter wrapping every 1 ms) at F_CPU = 64 MHz.
// SysTick->VAL therefore gives cycle-accurate timing with no extra hardware.
//
// WS2812B datasheet timings: T0H 0.35us +-150ns, T1H 0.7us +-150ns, bit
// period 1.25us. The values below leave headroom for the ~2-3 cycles of GPIO
// write latency plus up to ~6 cycles of loop granularity, and stay within
// tolerance. Cycle counts are derived from SystemCoreClock so the driver
// stays correct on both the default 64 MHz clock and the optional 128 MHz
// one (ARDUINO_NRF54_CPU_128M):
//   T0H: 20cy @64MHz = 312ns (+~140ns worst case -> 452ns, max 500ns)
//   T1H: 46cy @64MHz = 719ns (+~140ns worst case -> 859ns, max 850ns)
//   period: 80cy @64MHz = 1250ns

static const uint32_t kT0HNs = 312;   // 0.3125 us high time for a 0 bit
static const uint32_t kT1HNs = 719;   // ~0.72 us high time for a 1 bit
static const uint32_t kBitNs = 1250;  // 1.25 us bit period
static const uint32_t kLatchUs = 60;  // > 50 us reset gap between frames

static volatile uint32_t* s_wsOutSet = nullptr;  // GPIO OUTSET of the data pin
static volatile uint32_t* s_wsOutClr = nullptr;  // GPIO OUTCLR of the data pin
static uint32_t s_wsMask = 0;
static uint32_t s_lastFrameEndUs = 0;

static inline uint32_t systickElapsedCycles(uint32_t start, uint32_t load) {
    // SysTick counts down from LOAD to 0. "start - now" is the elapsed cycle
    // count, corrected for the wrap past zero (deltas are always < load+1).
    const uint32_t now = SysTick->VAL;
    uint32_t delta = start - now;
    if (now > start) {
        delta += load + 1;
    }
    return delta;
}

void ws2812_init(uint8_t pin) {
    uint8_t port = 0;
    uint8_t pinInPort = 0;
    if (!pinToPortPin(pin, &port, &pinInPort)) {
        return;
    }
    NRF_GPIO_Type* gpio;
    switch (port) {
        case 0:  gpio = NRF_P0; break;
        case 2:  gpio = NRF_P2; break;
        case 3:  gpio = NRF_P3; break;
        default: gpio = NRF_P1; break;
    }
    s_wsOutSet = &gpio->OUTSET;
    s_wsOutClr = &gpio->OUTCLR;
    s_wsMask = 1UL << pinInPort;
    s_lastFrameEndUs = 0;

    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
}

static void ws2812_show(const CRGB* leds, uint16_t count, uint8_t scale) {
    if (!s_wsOutSet || count == 0) {
        return;
    }
    const uint32_t load = SysTick->LOAD;

    // Bit timing in cycles, derived from the actual core clock (64 MHz by
    // default, 128 MHz with ARDUINO_NRF54_CPU_128M).
    const uint32_t cyclesPerUs =
        (SystemCoreClock >= 1000000UL) ? (SystemCoreClock / 1000000UL) : 64UL;
    const uint32_t t0hCycles = (kT0HNs * cyclesPerUs) / 1000UL;
    const uint32_t t1hCycles = (kT1HNs * cyclesPerUs) / 1000UL;
    const uint32_t bitCycles = (kBitNs * cyclesPerUs) / 1000UL;

    // Ensure the line has been low long enough for the frame latch before
    // starting a new frame (matters when show() is called back to back,
    // e.g. from FastLED.delay()).
    if (s_lastFrameEndUs) {
        const uint32_t sinceEnd = micros() - s_lastFrameEndUs;
        if (sinceEnd < kLatchUs) {
            delayMicroseconds(kLatchUs - sinceEnd);
        }
    }

    // Global interrupts off for the whole frame so no handler can stretch a
    // bit. A 10-LED frame is ~300us; SysTick keeps counting while masked and
    // the core's timebase accounts for the pending tick afterwards.
    __disable_irq();
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t grb[3] = {
            scale8(leds[i].g, scale),
            scale8(leds[i].r, scale),
            scale8(leds[i].b, scale),
        };
        for (uint8_t byteIndex = 0; byteIndex < 3; ++byteIndex) {
            uint8_t value = grb[byteIndex];
            for (uint8_t bit = 0; bit < 8; ++bit) {
                const uint32_t highCycles =
                    (value & 0x80) ? t1hCycles : t0hCycles;
                const uint32_t start = SysTick->VAL;
                *s_wsOutSet = s_wsMask;  // rising edge
                while (systickElapsedCycles(start, load) < highCycles) {
                }
                *s_wsOutClr = s_wsMask;  // falling edge
                while (systickElapsedCycles(start, load) < bitCycles) {
                }
                value <<= 1;
            }
        }
    }
    __enable_irq();
    s_lastFrameEndUs = micros();
}

// ============================================================== color utils

// Ported from FastLED 3.10.8 src/hsv2rgb.cpp.hpp (hsv2rgb_rainbow, C path,
// FASTLED_SCALE8_FIXED == 1, defaults Y1=1 Y2=0 G2=0 Gscale=0).
CRGB hsv2rgb_rainbow(const CHSV& hsv) {
    const uint8_t K255 = 255;
    const uint8_t K171 = 171;
    const uint8_t K170 = 170;
    const uint8_t K85 = 85;
    const uint8_t Y1 = 1;
    const uint8_t Y2 = 0;
    const uint8_t G2 = 0;
    const uint8_t Gscale = 0;

    uint8_t hue = hsv.hue;
    uint8_t sat = hsv.sat;
    uint8_t val = hsv.val;

    const uint8_t offset = hue & 0x1F;  // 0..31
    uint8_t offset8 = offset;
    offset8 <<= 3;

    const uint8_t third = scale8(offset8, (256 / 3));  // max = 85

    uint8_t r, g, b;

    if (!(hue & 0x80)) {
        if (!(hue & 0x40)) {
            if (!(hue & 0x20)) {
                // R -> O
                r = K255 - third;
                g = third;
                b = 0;
            } else {
                // O -> Y
                if (Y1) {
                    r = K171;
                    g = K85 + third;
                    b = 0;
                }
                if (Y2) {
                    r = K170 + third;
                    const uint8_t twothirds = scale8(offset8, ((256 * 2) / 3));
                    g = K85 + twothirds;
                    b = 0;
                }
            }
        } else {
            if (!(hue & 0x20)) {
                // Y -> G
                if (Y1) {
                    const uint8_t twothirds = scale8(offset8, ((256 * 2) / 3));
                    r = K171 - twothirds;
                    g = K170 + third;
                    b = 0;
                }
                if (Y2) {
                    r = K255 - offset8;
                    g = K255;
                    b = 0;
                }
            } else {
                // G -> A
                r = 0;
                g = K255 - third;
                b = third;
            }
        }
    } else {
        if (!(hue & 0x40)) {
            if (!(hue & 0x20)) {
                // A -> B
                const uint8_t twothirds = scale8(offset8, ((256 * 2) / 3));
                r = 0;
                g = K171 - twothirds;
                b = K85 + twothirds;
            } else {
                // B -> P
                r = third;
                g = 0;
                b = K255 - third;
            }
        } else {
            if (!(hue & 0x20)) {
                // P -- K
                r = K85 + third;
                g = 0;
                b = K171 - third;
            } else {
                // K -> R
                r = K170 + third;
                g = 0;
                b = K85 - third;
            }
        }
    }

    if (G2) {
        g = g >> 1;
    }
    if (Gscale) {
        g = scale8_video(g, Gscale);
    }

    if (sat != 255) {
        if (sat == 0) {
            r = 255;
            b = 255;
            g = 255;
        } else {
            uint8_t desat = 255 - sat;
            desat = scale8_video(desat, desat);
            const uint8_t satscale = 255 - desat;
            r = scale8(r, satscale);
            g = scale8(g, satscale);
            b = scale8(b, satscale);
            const uint8_t brightness_floor = desat;
            r += brightness_floor;
            g += brightness_floor;
            b += brightness_floor;
        }
    }

    if (val != 255) {
        val = scale8_video(val, val);
        if (val == 0) {
            r = 0;
            g = 0;
            b = 0;
        } else {
            r = scale8(r, val);
            g = scale8(g, val);
            b = scale8(b, val);
        }
    }

    return CRGB(r, g, b);
}

// fadeToBlackBy() = nscale8(255 - fadeBy) per pixel (FastLED pixelset.h).
void fadeToBlackBy(CRGB* leds, uint16_t numLeds, uint8_t fadeBy) {
    const uint8_t scale = (uint8_t)(255 - fadeBy);
    for (uint16_t i = 0; i < numLeds; ++i) {
        leds[i].r = scale8(leds[i].r, scale);
        leds[i].g = scale8(leds[i].g, scale);
        leds[i].b = scale8(leds[i].b, scale);
    }
}

void fill_solid(CRGB* leds, int numLeds, const CRGB& color) {
    for (int i = 0; i < numLeds; ++i) {
        leds[i] = color;
    }
}

// ================================================================= PRNG
// FastLED's 16-bit LCG (lib8tion/random8.h, default seed 1337).

static uint16_t s_rand16seed = 1337;

uint8_t random8() {
    s_rand16seed = (uint16_t)(s_rand16seed * 2053) + 13849;
    // Sum of the high and low bytes, for better mixing.
    return (uint8_t)(((uint8_t)(s_rand16seed & 0xFF)) +
                     ((uint8_t)(s_rand16seed >> 8)));
}

uint16_t random16() {
    s_rand16seed = (uint16_t)(s_rand16seed * 2053) + 13849;
    return s_rand16seed;
}

uint8_t random8(uint8_t lim) {
    const uint8_t r = random8();
    return (uint8_t)(((uint16_t)r * lim) >> 8);
}

uint16_t random16(uint16_t lim) {
    const uint16_t r = random16();
    return (uint16_t)(((uint32_t)r * lim) >> 16);
}

// =========================================================== CFastLED

CFastLED FastLED;

// FastLED's default WS2812 @ 5V power model: R 80mW, G 55mW, B 75mW per
// full channel, 5mW dark baseline per LED (power_mgt.h PowerModelRGB).
static uint32_t unscaledPowerMw(const CRGB* leds, uint16_t count) {
    uint32_t r = 0;
    uint32_t g = 0;
    uint32_t b = 0;
    for (uint16_t i = 0; i < count; ++i) {
        r += leds[i].r;
        g += leds[i].g;
        b += leds[i].b;
    }
    r = (r * 80) >> 8;
    g = (g * 55) >> 8;
    b = (b * 75) >> 8;
    return r + g + b + 5UL * count;
}

void CFastLED::show() {
    if (!mLeds || mCount == 0) {
        return;
    }
    uint8_t scale = mScale;
    if (mPowerLimitmW) {
        const uint32_t total = unscaledPowerMw(mLeds, mCount);
        if (total) {
            // Linear brightness response: cap the scale so the estimated
            // draw stays within the budget, like FastLED's limiter.
            const uint32_t maxScale = (mPowerLimitmW * 255UL) / total;
            if (maxScale < scale) {
                scale = (uint8_t)maxScale;
            }
        }
    }
    ws2812_show(mLeds, mCount, scale);
}

void CFastLED::delay(unsigned long ms) {
    const unsigned long start = millis();
    do {
        delay(1);  // make sure at least 1 ms passes, like FastLED
        show();
    } while ((millis() - start) < ms);
}

#endif  // ARDUINO_ARCH_NRF54L15CLEAN
