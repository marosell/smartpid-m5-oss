#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

using std::int8_t;
using std::int16_t;
using std::int32_t;
using std::uint8_t;
using std::uint16_t;
using std::uint32_t;

class String : public std::string {
public:
    String() = default;
    String(const char* s) : std::string(s ? s : "") {}
    String(const std::string& s) : std::string(s) {}
    String(int n) : std::string(std::to_string(n)) {}
    String(unsigned int n) : std::string(std::to_string(n)) {}
    String(long n) : std::string(std::to_string(n)) {}
    String(unsigned long n) : std::string(std::to_string(n)) {}
    String(float f, int dec = 2) {
        char b[32];
        snprintf(b, sizeof(b), "%.*f", dec, (double)f);
        assign(b);
    }

    int indexOf(const char* s) const {
        size_t pos = find(s);
        return pos == npos ? -1 : (int)pos;
    }
    int indexOf(char c) const {
        size_t pos = find(c);
        return pos == npos ? -1 : (int)pos;
    }
    int lastIndexOf(char c) const {
        size_t pos = rfind(c);
        return pos == npos ? -1 : (int)pos;
    }
    String substring(int from, int to = -1) const {
        if (to < 0) to = (int)size();
        return String(substr((size_t)from, (size_t)(to - from)));
    }
    int toInt() const { return size() ? std::stoi(*this) : 0; }
    float toFloat() const { return size() ? std::stof(*this) : 0.0f; }
    int length() const { return (int)size(); }
    size_t write(uint8_t c) {
        push_back((char)c);
        return 1;
    }
    size_t write(const uint8_t* data, size_t len) {
        if (data && len) append((const char*)data, len);
        return len;
    }
};

uint32_t millis();
void delay(uint32_t ms);

using std::abs;
using std::max;
using std::min;

template <typename T>
inline T constrain(T x, T lo, T hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

inline size_t strlcpy(char* dst, const char* src, size_t size) {
    const char* s = src ? src : "";
    size_t len = strlen(s);
    if (size > 0) {
        size_t n = len >= size ? size - 1 : len;
        memcpy(dst, s, n);
        dst[n] = '\0';
    }
    return len;
}

inline void configTime(long, int, const char*, const char* = nullptr) {}

inline bool getLocalTime(struct tm* info, uint32_t = 5000) {
    time_t t = time(nullptr);
    if (t < 0 || !localtime_r(&t, info)) return false;
    return true;
}

#define F(x) (x)
#define PROGMEM
#define PGM_P const char*
#ifndef pgm_read_byte
#define pgm_read_byte(p) (*(const uint8_t*)(uintptr_t)(p))
#endif
#ifndef pgm_read_word
#define pgm_read_word(p) (*(const uint16_t*)(uintptr_t)(p))
#endif
#ifndef pgm_read_dword
#define pgm_read_dword(p) (*(const uint32_t*)(uintptr_t)(p))
#endif

#define LOW 0
#define HIGH 1
#define INPUT 0x00
#define OUTPUT 0x01
#define INPUT_PULLUP 0x02
inline int digitalRead(int) { return LOW; }
inline void digitalWrite(int, int) {}
inline void pinMode(int, int) {}

inline long random(long hi) { return hi > 0 ? rand() % hi : 0; }
inline long random(long lo, long hi) { return lo + random(hi - lo); }

#define log_i(fmt, ...) do { printf("[I] " fmt "\n", ##__VA_ARGS__); } while (0)
#define log_w(fmt, ...) do { printf("[W] " fmt "\n", ##__VA_ARGS__); } while (0)
#define log_e(fmt, ...) do { printf("[E] " fmt "\n", ##__VA_ARGS__); } while (0)
#define log_d(...) do {} while (0)
