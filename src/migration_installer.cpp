#include "migration_installer.h"
#include "telemetry.h"

#ifndef DESKTOP_BUILD
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <new>
#endif

static constexpr uint32_t PACKAGE_HEADER_SIZE = 12;
static constexpr const char* PACKAGE_MAGIC = "PPMIG001";

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

#ifndef DESKTOP_BUILD
static void sha256ToHex(const uint8_t digest[32], char out[65]) {
    static const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2] = hex[(digest[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    out[64] = '\0';
}

static bool readExact(Stream& stream, uint8_t* out, size_t len, uint32_t timeoutMs = 15000) {
    size_t done = 0;
    uint32_t lastProgressMs = millis();
    while (done < len) {
        int available = stream.available();
        if (available > 0) {
            size_t want = len - done;
            size_t chunk = (size_t)available;
            if (chunk > want) chunk = want;
            int got = stream.readBytes(out + done, chunk);
            if (got > 0) {
                done += (size_t)got;
                lastProgressMs = millis();
                continue;
            }
        }
        if (millis() - lastProgressMs > timeoutMs) return false;
        delay(5);
    }
    return true;
}

static bool readAndHash(Stream& stream,
                        size_t len,
                        mbedtls_sha256_context& packageCtx,
                        mbedtls_sha256_context* artifactCtx,
                        uint32_t& bytesDone,
                        uint32_t bytesTotal,
                        TelemetryPublisher* telemetry,
                        const char* phase,
                        const MigrationInstallRequest& request) {
    uint8_t buf[1024];
    size_t remaining = len;
    uint32_t lastEventMs = 0;
    while (remaining > 0) {
        size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
        if (!readExact(stream, buf, chunk)) return false;
        mbedtls_sha256_update(&packageCtx, buf, chunk);
        if (artifactCtx) mbedtls_sha256_update(artifactCtx, buf, chunk);
        remaining -= chunk;
        bytesDone += (uint32_t)chunk;
        if (telemetry && (millis() - lastEventMs > 1000 || remaining == 0)) {
            telemetry->publishMigrationInstallStatus(phase,
                                                     "validating",
                                                     nullptr,
                                                     request.packageUrl,
                                                     request.packageSha256,
                                                     bytesDone,
                                                     bytesTotal);
            lastEventMs = millis();
        }
    }
    return true;
}

static uint32_t readU32Le(const uint8_t* bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

struct ExpectedArtifact {
    const char* role;
    uint32_t offset;
    uint32_t maxSize;
};

static constexpr ExpectedArtifact EXPECTED_ARTIFACTS[] = {
    {"proofpro_app0", 0x10000, 0x1f0000},
    {"smartpid_oem_app1", 0x200000, 0x1f0000},
    {"partition_table", 0x8000, 0x0c00},
    {"bootloader", 0x1000, 0x7000},
    {"otadata_boot_app0", 0xe000, 0x2000},
};

static MigrationInstallResult validatePackageStream(const MigrationInstallRequest& request,
                                                    TelemetryPublisher* telemetry) {
    HTTPClient http;
    http.setTimeout(15000);
    if (!http.begin(request.packageUrl)) {
        if (telemetry) {
            telemetry->publishMigrationInstallStatus("download",
                                                     "rejected",
                                                     "http_begin_failed",
                                                     request.packageUrl,
                                                     request.packageSha256);
        }
        return MigrationInstallResult::DOWNLOAD_FAILED;
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        if (telemetry) {
            telemetry->publishMigrationInstallStatus("download",
                                                     "rejected",
                                                     "http_status",
                                                     request.packageUrl,
                                                     request.packageSha256);
        }
        http.end();
        return MigrationInstallResult::DOWNLOAD_FAILED;
    }

    const int contentLength = http.getSize();
    const uint32_t bytesTotal = contentLength > 0 ? (uint32_t)contentLength : 0;
    WiFiClient* stream = http.getStreamPtr();
    if (!stream) {
        http.end();
        return MigrationInstallResult::DOWNLOAD_FAILED;
    }

    mbedtls_sha256_context packageCtx;
    mbedtls_sha256_init(&packageCtx);
    mbedtls_sha256_starts(&packageCtx, 0);
    uint32_t bytesDone = 0;

    uint8_t header[PACKAGE_HEADER_SIZE] = {};
    if (!readExact(*stream, header, sizeof(header))) {
        mbedtls_sha256_free(&packageCtx);
        http.end();
        return MigrationInstallResult::DOWNLOAD_FAILED;
    }
    mbedtls_sha256_update(&packageCtx, header, sizeof(header));
    bytesDone += sizeof(header);

    if (memcmp(header, PACKAGE_MAGIC, 8) != 0) {
        mbedtls_sha256_free(&packageCtx);
        http.end();
        if (telemetry) {
            telemetry->publishMigrationInstallStatus("package_header",
                                                     "rejected",
                                                     "bad_magic",
                                                     request.packageUrl,
                                                     request.packageSha256,
                                                     bytesDone,
                                                     bytesTotal);
        }
        return MigrationInstallResult::PACKAGE_INVALID;
    }

    const uint32_t manifestLen = readU32Le(header + 8);
    if (manifestLen == 0 || manifestLen > 8192) {
        mbedtls_sha256_free(&packageCtx);
        http.end();
        if (telemetry) {
            telemetry->publishMigrationInstallStatus("package_header",
                                                     "rejected",
                                                     "bad_manifest_length",
                                                     request.packageUrl,
                                                     request.packageSha256,
                                                     bytesDone,
                                                     bytesTotal);
        }
        return MigrationInstallResult::PACKAGE_INVALID;
    }

    char* manifest = new (std::nothrow) char[manifestLen + 1];
    if (!manifest) {
        mbedtls_sha256_free(&packageCtx);
        http.end();
        return MigrationInstallResult::DOWNLOAD_FAILED;
    }
    if (!readAndHash(*stream, manifestLen, packageCtx, nullptr, bytesDone, bytesTotal,
                     telemetry, "manifest", request)) {
        delete[] manifest;
        mbedtls_sha256_free(&packageCtx);
        http.end();
        return MigrationInstallResult::DOWNLOAD_FAILED;
    }
    manifest[manifestLen] = '\0';

    JsonDocument doc;
    DeserializationError jsonErr = deserializeJson(doc, manifest);
    delete[] manifest;
    if (jsonErr) {
        mbedtls_sha256_free(&packageCtx);
        http.end();
        if (telemetry) {
            telemetry->publishMigrationInstallStatus("manifest",
                                                     "rejected",
                                                     "manifest_json_error",
                                                     request.packageUrl,
                                                     request.packageSha256,
                                                     bytesDone,
                                                     bytesTotal);
        }
        return MigrationInstallResult::PACKAGE_INVALID;
    }

    if (strcmp(doc["schema"] | "", "proofpro_oem_layout_migration_manifest") != 0 ||
        (doc["schema_version"] | 0) != 1 ||
        strcmp(doc["boot_slot"] | "", "app0") != 0) {
        mbedtls_sha256_free(&packageCtx);
        http.end();
        if (telemetry) {
            telemetry->publishMigrationInstallStatus("manifest",
                                                     "rejected",
                                                     "manifest_schema_mismatch",
                                                     request.packageUrl,
                                                     request.packageSha256,
                                                     bytesDone,
                                                     bytesTotal);
        }
        return MigrationInstallResult::PACKAGE_INVALID;
    }

    JsonArray artifacts = doc["artifacts"].as<JsonArray>();
    constexpr size_t expectedArtifactCount = sizeof(EXPECTED_ARTIFACTS) / sizeof(EXPECTED_ARTIFACTS[0]);
    if (artifacts.isNull() || artifacts.size() != expectedArtifactCount) {
        mbedtls_sha256_free(&packageCtx);
        http.end();
        if (telemetry) {
            telemetry->publishMigrationInstallStatus("manifest",
                                                     "rejected",
                                                     "bad_artifact_count",
                                                     request.packageUrl,
                                                     request.packageSha256,
                                                     bytesDone,
                                                     bytesTotal);
        }
        return MigrationInstallResult::PACKAGE_INVALID;
    }

    size_t artifactIndex = 0;
    for (JsonObject artifact : artifacts) {
        const ExpectedArtifact& expected = EXPECTED_ARTIFACTS[artifactIndex++];
        const char* role = artifact["role"] | "";
        const char* expectedSha = artifact["sha256"] | "";
        uint32_t size = artifact["size"] | 0;
        uint32_t offset = artifact["offset"] | 0;
        if (strcmp(role, expected.role) != 0 ||
            offset != expected.offset ||
            size == 0 ||
            size > expected.maxSize ||
            !validSha256Hex(expectedSha)) {
            mbedtls_sha256_free(&packageCtx);
            http.end();
            if (telemetry) {
                telemetry->publishMigrationInstallStatus("artifact",
                                                         "rejected",
                                                         "bad_artifact_metadata",
                                                         request.packageUrl,
                                                         request.packageSha256,
                                                         bytesDone,
                                                         bytesTotal);
            }
            return MigrationInstallResult::PACKAGE_INVALID;
        }

        mbedtls_sha256_context artifactCtx;
        mbedtls_sha256_init(&artifactCtx);
        mbedtls_sha256_starts(&artifactCtx, 0);
        bool ok = readAndHash(*stream, size, packageCtx, &artifactCtx, bytesDone, bytesTotal,
                              telemetry, role, request);
        uint8_t artifactDigest[32] = {};
        char artifactHex[65] = {};
        mbedtls_sha256_finish(&artifactCtx, artifactDigest);
        mbedtls_sha256_free(&artifactCtx);
        if (!ok) {
            mbedtls_sha256_free(&packageCtx);
            http.end();
            return MigrationInstallResult::DOWNLOAD_FAILED;
        }
        sha256ToHex(artifactDigest, artifactHex);
        if (strcmp(artifactHex, expectedSha) != 0) {
            mbedtls_sha256_free(&packageCtx);
            http.end();
            if (telemetry) {
                telemetry->publishMigrationInstallStatus(role,
                                                         "rejected",
                                                         "artifact_sha_mismatch",
                                                         request.packageUrl,
                                                         request.packageSha256,
                                                         bytesDone,
                                                         bytesTotal);
            }
            return MigrationInstallResult::PACKAGE_INVALID;
        }
    }

    if (contentLength > 0 && bytesDone != (uint32_t)contentLength) {
        mbedtls_sha256_free(&packageCtx);
        http.end();
        if (telemetry) {
            telemetry->publishMigrationInstallStatus("package",
                                                     "rejected",
                                                     "package_size_mismatch",
                                                     request.packageUrl,
                                                     request.packageSha256,
                                                     bytesDone,
                                                     bytesTotal);
        }
        return MigrationInstallResult::PACKAGE_INVALID;
    }

    uint8_t packageDigest[32] = {};
    char packageHex[65] = {};
    mbedtls_sha256_finish(&packageCtx, packageDigest);
    mbedtls_sha256_free(&packageCtx);
    http.end();
    sha256ToHex(packageDigest, packageHex);
    if (strcmp(packageHex, request.packageSha256) != 0) {
        if (telemetry) {
            telemetry->publishMigrationInstallStatus("package",
                                                     "rejected",
                                                     "package_sha_mismatch",
                                                     request.packageUrl,
                                                     request.packageSha256,
                                                     bytesDone,
                                                     bytesTotal);
        }
        return MigrationInstallResult::PACKAGE_INVALID;
    }

    if (telemetry) {
        telemetry->publishMigrationInstallStatus("package",
                                                 "validated",
                                                 "download_verified",
                                                 request.packageUrl,
                                                 request.packageSha256,
                                                 bytesDone,
                                                 bytesTotal);
    }
    return MigrationInstallResult::ACCEPTED;
}
#endif

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

#ifdef DESKTOP_BUILD
    return MigrationInstallResult::UNSAFE_STATE;
#else
    MigrationInstallResult validation = validatePackageStream(request, telemetry);
    if (validation != MigrationInstallResult::ACCEPTED) return validation;

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
