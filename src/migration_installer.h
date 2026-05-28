#pragma once

#include <Arduino.h>

class TelemetryPublisher;

struct MigrationInstallRequest {
    const char* packageUrl = nullptr;
    const char* packageSha256 = nullptr;
};

enum class MigrationInstallResult {
    ACCEPTED,
    INVALID_REQUEST,
    UNSAFE_STATE,
    WRITES_DISABLED,
};

MigrationInstallResult migrationInstallOemLayout(const MigrationInstallRequest& request,
                                                 TelemetryPublisher* telemetry);
