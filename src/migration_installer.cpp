#include "migration_installer.h"
#include "telemetry.h"

#ifndef DESKTOP_BUILD
#include <esp_ota_ops.h>
#include <esp_partition.h>
#endif

static bool isHexChar(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static bool validSha256Hex(const char* value) {
    if (!value || strlen(value) != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        if (!isHexChar(value[i])) return false;
    }
    return true;
}

static bool validPackageUrl(const char* value) {
    if (!value || value[0] == '\0') return false;
    return strncmp(value, "http://", 7) == 0;
}

static bool runningFromCurrentHighApp1() {
#ifdef DESKTOP_BUILD
    return false;
#else
    const esp_partition_t* running = esp_ota_get_running_partition();
    return running && running->address == 0x650000 && running->size == 0x640000;
#endif
}

MigrationInstallResult migrationInstallOemLayout(const MigrationInstallRequest& request,
                                                 TelemetryPublisher* telemetry) {
    if (!validPackageUrl(request.packageUrl)) {
        if (telemetry) {
            telemetry->publishMigrationInstallStatus("validate_request",
                                                     "rejected",
                                                     "invalid_package_url",
                                                     request.packageUrl,
                                                     request.packageSha256);
        }
        return MigrationInstallResult::INVALID_REQUEST;
    }
    if (!validSha256Hex(request.packageSha256)) {
        if (telemetry) {
            telemetry->publishMigrationInstallStatus("validate_request",
                                                     "rejected",
                                                     "invalid_package_sha256",
                                                     request.packageUrl,
                                                     request.packageSha256);
        }
        return MigrationInstallResult::INVALID_REQUEST;
    }
    if (!runningFromCurrentHighApp1()) {
        if (telemetry) {
            telemetry->publishMigrationInstallStatus("preflight",
                                                     "rejected",
                                                     "not_running_from_high_app1",
                                                     request.packageUrl,
                                                     request.packageSha256);
        }
        return MigrationInstallResult::UNSAFE_STATE;
    }

#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_INSTALL) && !defined(DESKTOP_BUILD)
    // The destructive writer is intentionally not implemented yet. This block is
    // the only place where HTTP streaming and esp_partition erase/write calls
    // should be added after hardware failure modes are reviewed.
    if (telemetry) {
        telemetry->publishMigrationInstallStatus("writer",
                                                 "rejected",
                                                 "writer_not_implemented",
                                                 request.packageUrl,
                                                 request.packageSha256);
    }
    return MigrationInstallResult::WRITES_DISABLED;
#else
    if (telemetry) {
        telemetry->publishMigrationInstallStatus("writer",
                                                 "rejected",
                                                 "writes_not_enabled",
                                                 request.packageUrl,
                                                 request.packageSha256);
    }
    return MigrationInstallResult::WRITES_DISABLED;
#endif
}
