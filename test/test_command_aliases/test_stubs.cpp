#include "display.h"

DisplayManager display;

uint32_t millis() {
    return 1000;
}

void delay(uint32_t) {}

void DisplayManager::notifyMqttChanged() {}
