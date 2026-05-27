#pragma once

#define DIRECT 0
#define REVERSE 1
#define AUTOMATIC 1
#define MANUAL 0

class PID {
public:
    PID(double*, double*, double*, double, double, double, int) {}
    void SetOutputLimits(double, double) {}
    void SetMode(int) {}
    void SetSampleTime(int) {}
    void SetTunings(double, double, double) {}
    bool Compute() { return false; }
};

