#pragma once

#include <Arduino.h>

class JsonDocument {};
inline size_t serializeJson(const JsonDocument&, String&) { return 0; }

