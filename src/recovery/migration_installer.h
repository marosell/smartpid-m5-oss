#pragma once

#include <Arduino.h>

class TelemetryPublisher;

struct MigrationInstallRequest {
    const char* packageUrl = nullptr;
    const char* packageSha256 = nullptr;
    const char* writeStage = nullptr;
};

enum class MigrationInstallResult {
    ACCEPTED,
    INVALID_REQUEST,
    INVALID_WRITE_STAGE,
    UNSAFE_STATE,
    DOWNLOAD_FAILED,
    PACKAGE_INVALID,
    FLASH_WRITE_FAILED,
    FLASH_VERIFY_FAILED,
    WRITES_DISABLED,
};

MigrationInstallResult migrationInstallOemLayout(const MigrationInstallRequest& request,
                                                 TelemetryPublisher* telemetry);

MigrationInstallResult migrationRestoreSmartPidApp1(const MigrationInstallRequest& request,
                                                    TelemetryPublisher* telemetry);
