#include "migration_installer.h"
#include "telemetry.h"

#ifndef DESKTOP_BUILD
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_flash.h>
#include <esp_flash_internal.h>
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

static const char* requestedWriteStage(const MigrationInstallRequest& request) {
    return (request.writeStage && request.writeStage[0] != '\0') ? request.writeStage : "validate_only";
}

static bool validWriteStage(const char* value) {
    return value &&
           (strcmp(value, "validate_only") == 0 ||
            strcmp(value, "apps") == 0 ||
            strcmp(value, "metadata") == 0 ||
            strcmp(value, "all") == 0);
}

static bool runningFromCurrentHighApp1() {
#ifdef DESKTOP_BUILD
    return false;
#else
    const esp_partition_t* running = esp_ota_get_running_partition();
    return running && running->address == 0x650000 && running->size == 0x640000;
#endif
}

static bool runningFromOemLayoutApp0() {
#ifdef DESKTOP_BUILD
    return false;
#else
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* app0 =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_APP_OTA_0,
                                 nullptr);
    const esp_partition_t* app1 =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                 ESP_PARTITION_SUBTYPE_APP_OTA_1,
                                 nullptr);
    return running && app0 && app1 &&
           running->address == 0x10000 && running->size == 0x1f0000 &&
           app0->address == 0x10000 && app0->size == 0x1f0000 &&
           app1->address == 0x200000 && app1->size == 0x1f0000;
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

struct ExpectedArtifact {
    const char* role;
    uint32_t offset;
    uint32_t maxSize;
};

#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
struct BufferedMetadataArtifact {
    const ExpectedArtifact* expected = nullptr;
    uint8_t* data = nullptr;
    uint32_t size = 0;
    char sha256[65] = {};
};
#endif

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
        delay(1);
    }
    return true;
}

static MigrationInstallResult readHashAndMaybeWrite(Stream& stream,
                                                    const ExpectedArtifact& artifact,
                                                    size_t len,
                                                    bool writeArtifact,
#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
                                                    BufferedMetadataArtifact* bufferedMetadata,
#endif
                                                    mbedtls_sha256_context& packageCtx,
                                                    mbedtls_sha256_context& artifactCtx,
                                                    uint32_t& bytesDone,
                                                    uint32_t bytesTotal,
                                                    TelemetryPublisher* telemetry,
                                                    const MigrationInstallRequest& request) {
    uint8_t buf[1024];
    size_t remaining = len;
    uint32_t artifactOffset = 0;
    uint32_t lastEventMs = 0;

    while (remaining > 0) {
        size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
        if (!readExact(stream, buf, chunk)) return MigrationInstallResult::DOWNLOAD_FAILED;

        mbedtls_sha256_update(&packageCtx, buf, chunk);
        mbedtls_sha256_update(&artifactCtx, buf, chunk);

#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_INSTALL) || defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
        if (writeArtifact) {
#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
            if (bufferedMetadata) {
                memcpy(bufferedMetadata->data + artifactOffset, buf, chunk);
            } else
#endif
            {
                if (esp_flash_write(nullptr, buf, artifact.offset + artifactOffset, chunk) != ESP_OK) {
                    return MigrationInstallResult::FLASH_WRITE_FAILED;
                }
            }
        }
#else
        (void)writeArtifact;
#endif

        remaining -= chunk;
        artifactOffset += (uint32_t)chunk;
        bytesDone += (uint32_t)chunk;
        if (telemetry && (millis() - lastEventMs > 1000 || remaining == 0)) {
            telemetry->publishMigrationInstallStatus(artifact.role,
                                                     writeArtifact ? "writing" : "validating",
                                                     nullptr,
                                                     request.packageUrl,
                                                     request.packageSha256,
                                                     bytesDone,
                                                     bytesTotal,
                                                     requestedWriteStage(request));
            lastEventMs = millis();
        }
        delay(1);
    }

    return MigrationInstallResult::ACCEPTED;
}

static uint32_t readU32Le(const uint8_t* bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static constexpr ExpectedArtifact EXPECTED_ARTIFACTS[] = {
    {"proofpro_app0", 0x10000, 0x1f0000},
    {"smartpid_oem_app1", 0x200000, 0x1f0000},
    {"partition_table", 0x8000, 0x0c00},
    {"bootloader", 0x1000, 0x7000},
    {"otadata_boot_app0", 0xe000, 0x2000},
};

static bool isAppArtifact(const char* role) {
    return strcmp(role, "proofpro_app0") == 0 ||
           strcmp(role, "smartpid_oem_app1") == 0;
}

static bool isMetadataArtifact(const char* role) {
    return strcmp(role, "partition_table") == 0 ||
           strcmp(role, "bootloader") == 0 ||
           strcmp(role, "otadata_boot_app0") == 0;
}

#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
static void freeMetadataBuffers(BufferedMetadataArtifact* buffers, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        delete[] buffers[i].data;
        buffers[i].data = nullptr;
        buffers[i].size = 0;
        buffers[i].expected = nullptr;
        buffers[i].sha256[0] = '\0';
    }
}

static BufferedMetadataArtifact* findBufferedMetadata(BufferedMetadataArtifact* buffers,
                                                      size_t count,
                                                      const char* role) {
    for (size_t i = 0; i < count; ++i) {
        if (buffers[i].expected && strcmp(buffers[i].expected->role, role) == 0) {
            return &buffers[i];
        }
    }
    return nullptr;
}
#endif

#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_INSTALL) || defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
static bool eraseFlashRegion(uint32_t offset, uint32_t size) {
    constexpr uint32_t sectorSize = 0x1000;
    const uint32_t eraseSize = (size + sectorSize - 1) & ~(sectorSize - 1);
    for (uint32_t done = 0; done < eraseSize; done += sectorSize) {
        if (esp_flash_erase_region(nullptr, offset + done, sectorSize) != ESP_OK) {
            return false;
        }
        delay(1);
    }
    return true;
}

static bool hashFlashRegion(uint32_t offset, uint32_t size, char outHex[65]) {
    uint8_t buf[1024];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    uint32_t done = 0;
    while (done < size) {
        uint32_t chunk = size - done;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        if (esp_flash_read(nullptr, buf, offset + done, chunk) != ESP_OK) {
            mbedtls_sha256_free(&ctx);
            return false;
        }
        mbedtls_sha256_update(&ctx, buf, chunk);
        done += chunk;
        delay(1);
    }

    uint8_t digest[32] = {};
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
    sha256ToHex(digest, outHex);
    return true;
}

#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
static bool writeFlashBuffer(uint32_t offset, const uint8_t* data, uint32_t size) {
    uint32_t done = 0;
    while (done < size) {
        uint32_t chunk = size - done;
        if (chunk > 1024) chunk = 1024;
        if (esp_flash_write(nullptr, data + done, offset + done, chunk) != ESP_OK) {
            return false;
        }
        done += chunk;
        delay(1);
    }
    return true;
}

static MigrationInstallResult writeBufferedMetadata(BufferedMetadataArtifact* buffers,
                                                    size_t count,
                                                    TelemetryPublisher* telemetry,
                                                    const MigrationInstallRequest& request) {
    static constexpr const char* WRITE_ORDER[] = {
        "bootloader",
        "partition_table",
        "otadata_boot_app0",
    };

    for (const char* role : WRITE_ORDER) {
        if (!findBufferedMetadata(buffers, count, role)) {
            return MigrationInstallResult::PACKAGE_INVALID;
        }
    }

    if (esp_flash_set_dangerous_write_protection(esp_flash_default_chip, false) != ESP_OK) {
        Serial.println("[MIGRATION] failed to disable dangerous-write protection");
        return MigrationInstallResult::FLASH_WRITE_FAILED;
    }

    if (telemetry) {
        telemetry->publishMigrationInstallStatus("metadata_critical",
                                                 "writing",
                                                 "critical_flash_write",
                                                 request.packageUrl,
                                                 request.packageSha256,
                                                 0,
                                                 0,
                                                 requestedWriteStage(request));
    }
    delay(500);

    for (const char* role : WRITE_ORDER) {
        BufferedMetadataArtifact* item = findBufferedMetadata(buffers, count, role);
        if (!item || !item->expected || !item->data) {
            return MigrationInstallResult::PACKAGE_INVALID;
        }
        Serial.printf("[MIGRATION] metadata write begin: %s offset=0x%lx size=%lu\n",
                      role,
                      static_cast<unsigned long>(item->expected->offset),
                      static_cast<unsigned long>(item->size));
        if (!eraseFlashRegion(item->expected->offset, item->expected->maxSize)) {
            Serial.printf("[MIGRATION] metadata erase failed: %s\n", role);
            esp_flash_set_dangerous_write_protection(esp_flash_default_chip, true);
            return MigrationInstallResult::FLASH_WRITE_FAILED;
        }
        if (!writeFlashBuffer(item->expected->offset, item->data, item->size)) {
            Serial.printf("[MIGRATION] metadata write failed: %s\n", role);
            esp_flash_set_dangerous_write_protection(esp_flash_default_chip, true);
            return MigrationInstallResult::FLASH_WRITE_FAILED;
        }
        char flashHex[65] = {};
        if (!hashFlashRegion(item->expected->offset, item->size, flashHex) ||
            strcmp(flashHex, item->sha256) != 0) {
            Serial.printf("[MIGRATION] metadata verify failed: %s\n", role);
            esp_flash_set_dangerous_write_protection(esp_flash_default_chip, true);
            return MigrationInstallResult::FLASH_VERIFY_FAILED;
        }
        Serial.printf("[MIGRATION] metadata write verified: %s\n", role);
    }

    Serial.println("[MIGRATION] metadata complete; restarting into OEM layout");
    delay(250);
    ESP.restart();
    return MigrationInstallResult::ACCEPTED;
}
#endif
#endif

static MigrationInstallResult validatePackageStream(const MigrationInstallRequest& request,
                                                    TelemetryPublisher* telemetry,
                                                    bool enableAppWrites,
                                                    bool enableMetadataWrites,
                                                    bool enableSmartPidApp1Write = false) {
    const char* writeStage = requestedWriteStage(request);
#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_INSTALL)
    const bool writeApps = enableAppWrites && strcmp(writeStage, "apps") == 0;
#else
    (void)enableAppWrites;
    const bool writeApps = false;
#endif
#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
    const bool writeMetadata = enableMetadataWrites && strcmp(writeStage, "metadata") == 0;
#else
    (void)enableMetadataWrites;
    const bool writeMetadata = false;
#endif
#if defined(PROOFPRO_ENABLE_OEM_APP_RESTORE)
    const bool writeSmartPidApp1 = enableSmartPidApp1Write;
#else
    (void)enableSmartPidApp1Write;
    const bool writeSmartPidApp1 = false;
#endif

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
#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
    BufferedMetadataArtifact metadataBuffers[3];
    size_t metadataBufferCount = 0;
#endif

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
    size_t manifestDone = 0;
    uint32_t lastManifestEventMs = 0;
    while (manifestDone < manifestLen) {
        const size_t remaining = manifestLen - manifestDone;
        const size_t chunk = remaining < 1024 ? remaining : 1024;
        if (!readExact(*stream, reinterpret_cast<uint8_t*>(manifest + manifestDone), chunk)) {
            delete[] manifest;
            mbedtls_sha256_free(&packageCtx);
            http.end();
            return MigrationInstallResult::DOWNLOAD_FAILED;
        }
        mbedtls_sha256_update(&packageCtx,
                              reinterpret_cast<const uint8_t*>(manifest + manifestDone),
                              chunk);
        manifestDone += chunk;
        bytesDone += (uint32_t)chunk;
        if (telemetry && (millis() - lastManifestEventMs > 1000 || manifestDone == manifestLen)) {
            telemetry->publishMigrationInstallStatus("manifest",
                                                     "validating",
                                                     nullptr,
                                                     request.packageUrl,
                                                     request.packageSha256,
                                                     bytesDone,
                                                     bytesTotal);
            lastManifestEventMs = millis();
        }
        delay(1);
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

        const bool writeArtifact = (writeApps && isAppArtifact(role)) ||
                                   (writeSmartPidApp1 && strcmp(role, "smartpid_oem_app1") == 0) ||
                                   (writeMetadata && isMetadataArtifact(role));
#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
        BufferedMetadataArtifact* bufferedMetadata = nullptr;
        if (writeMetadata && isMetadataArtifact(role)) {
            if (metadataBufferCount >= 3) {
                mbedtls_sha256_free(&packageCtx);
                http.end();
                freeMetadataBuffers(metadataBuffers, 3);
                return MigrationInstallResult::PACKAGE_INVALID;
            }
            bufferedMetadata = &metadataBuffers[metadataBufferCount++];
            bufferedMetadata->expected = &expected;
            bufferedMetadata->size = size;
            strncpy(bufferedMetadata->sha256, expectedSha, sizeof(bufferedMetadata->sha256) - 1);
            bufferedMetadata->data = new (std::nothrow) uint8_t[size];
            if (!bufferedMetadata->data) {
                mbedtls_sha256_free(&packageCtx);
                http.end();
                freeMetadataBuffers(metadataBuffers, 3);
                return MigrationInstallResult::DOWNLOAD_FAILED;
            }
        }
#endif
#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_INSTALL) || defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
        if (writeArtifact && !writeMetadata) {
            if (telemetry) {
                telemetry->publishMigrationInstallStatus(role,
                                                         "erasing",
                                                         nullptr,
                                                         request.packageUrl,
                                                         request.packageSha256,
                                                         bytesDone,
                                                         bytesTotal,
                                                         writeStage);
            }
            if (!eraseFlashRegion(expected.offset, expected.maxSize)) {
                mbedtls_sha256_free(&packageCtx);
                http.end();
                if (telemetry) {
                    telemetry->publishMigrationInstallStatus(role,
                                                             "rejected",
                                                             "flash_erase_failed",
                                                             request.packageUrl,
                                                             request.packageSha256,
                                                             bytesDone,
                                                             bytesTotal,
                                                             writeStage);
                }
                return MigrationInstallResult::FLASH_WRITE_FAILED;
            }
        }
#endif

        mbedtls_sha256_context artifactCtx;
        mbedtls_sha256_init(&artifactCtx);
        mbedtls_sha256_starts(&artifactCtx, 0);
        MigrationInstallResult readResult = readHashAndMaybeWrite(*stream,
                                                                  expected,
                                                                  size,
                                                                  writeArtifact,
#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
                                                                  bufferedMetadata,
#endif
                                                                  packageCtx,
                                                                  artifactCtx,
                                                                  bytesDone,
                                                                  bytesTotal,
                                                                  telemetry,
                                                                  request);
        uint8_t artifactDigest[32] = {};
        char artifactHex[65] = {};
        mbedtls_sha256_finish(&artifactCtx, artifactDigest);
        mbedtls_sha256_free(&artifactCtx);
        if (readResult != MigrationInstallResult::ACCEPTED) {
            mbedtls_sha256_free(&packageCtx);
            http.end();
#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
            freeMetadataBuffers(metadataBuffers, 3);
#endif
            return readResult;
        }
        sha256ToHex(artifactDigest, artifactHex);
        if (strcmp(artifactHex, expectedSha) != 0) {
            mbedtls_sha256_free(&packageCtx);
            http.end();
#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
            freeMetadataBuffers(metadataBuffers, 3);
#endif
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

#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_INSTALL) || defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
        if (writeArtifact && !writeMetadata) {
            char flashHex[65] = {};
            if (!hashFlashRegion(expected.offset, size, flashHex)) {
                mbedtls_sha256_free(&packageCtx);
                http.end();
#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
                freeMetadataBuffers(metadataBuffers, 3);
#endif
                if (telemetry) {
                    telemetry->publishMigrationInstallStatus(role,
                                                             "rejected",
                                                             "flash_readback_failed",
                                                             request.packageUrl,
                                                             request.packageSha256,
                                                             bytesDone,
                                                             bytesTotal,
                                                             writeStage);
                }
                return MigrationInstallResult::FLASH_VERIFY_FAILED;
            }
            if (strcmp(flashHex, expectedSha) != 0) {
                mbedtls_sha256_free(&packageCtx);
                http.end();
#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
                freeMetadataBuffers(metadataBuffers, 3);
#endif
                if (telemetry) {
                    telemetry->publishMigrationInstallStatus(role,
                                                             "rejected",
                                                             "flash_verify_sha_mismatch",
                                                             request.packageUrl,
                                                             request.packageSha256,
                                                             bytesDone,
                                                             bytesTotal,
                                                             writeStage);
                }
                return MigrationInstallResult::FLASH_VERIFY_FAILED;
            }
            if (telemetry) {
                telemetry->publishMigrationInstallStatus(role,
                                                         "verified",
                                                         "flash_readback_verified",
                                                         request.packageUrl,
                                                         request.packageSha256,
                                                         bytesDone,
                                                         bytesTotal,
                                                         writeStage);
            }
        }
#endif
    }

    if (contentLength > 0 && bytesDone != (uint32_t)contentLength) {
        mbedtls_sha256_free(&packageCtx);
        http.end();
#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
        freeMetadataBuffers(metadataBuffers, 3);
#endif
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
#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
        freeMetadataBuffers(metadataBuffers, 3);
#endif
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

#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
    if (writeMetadata) {
        MigrationInstallResult metadataResult =
            writeBufferedMetadata(metadataBuffers, metadataBufferCount, telemetry, request);
        freeMetadataBuffers(metadataBuffers, 3);
        return metadataResult;
    }
    freeMetadataBuffers(metadataBuffers, 3);
#endif

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
    const char* writeStage = requestedWriteStage(request);
    if (!validWriteStage(writeStage)) {
        if (telemetry) {
            telemetry->publishMigrationInstallStatus("validate_request",
                                                     "rejected",
                                                     "invalid_write_stage",
                                                     request.packageUrl,
                                                     request.packageSha256);
        }
        return MigrationInstallResult::INVALID_WRITE_STAGE;
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
    MigrationInstallResult validation = validatePackageStream(request, telemetry, false, false);
    if (validation != MigrationInstallResult::ACCEPTED) return validation;

    if (strcmp(writeStage, "validate_only") == 0) {
        if (telemetry) {
            telemetry->publishMigrationInstallStatus("writer",
                                                     "validated",
                                                     "validate_only",
                                                     request.packageUrl,
                                                     request.packageSha256,
                                                     0,
                                                     0,
                                                     writeStage);
        }
        return MigrationInstallResult::ACCEPTED;
    }

#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_INSTALL)
    if (strcmp(writeStage, "apps") == 0) {
        MigrationInstallResult writeResult = validatePackageStream(request, telemetry, true, false);
        if (writeResult != MigrationInstallResult::ACCEPTED) return writeResult;
        if (telemetry) {
            telemetry->publishMigrationInstallStatus("writer",
                                                     "verified",
                                                     "apps_written",
                                                     request.packageUrl,
                                                     request.packageSha256,
                                                     0,
                                                     0,
                                                     writeStage);
        }
        return MigrationInstallResult::ACCEPTED;
    }
#endif

#if defined(PROOFPRO_ENABLE_OEM_LAYOUT_METADATA_INSTALL)
    if (strcmp(writeStage, "metadata") == 0) {
        MigrationInstallResult writeResult = validatePackageStream(request, telemetry, false, true);
        if (writeResult != MigrationInstallResult::ACCEPTED) return writeResult;
        if (telemetry) {
            telemetry->publishMigrationInstallStatus("writer",
                                                     "verified",
                                                     "metadata_written",
                                                     request.packageUrl,
                                                     request.packageSha256,
                                                     0,
                                                     0,
                                                     writeStage);
        }
        return MigrationInstallResult::ACCEPTED;
    }
#endif

    if (telemetry) {
        telemetry->publishMigrationInstallStatus("writer",
                                                 "rejected",
                                                 "writes_not_enabled",
                                                 request.packageUrl,
                                                 request.packageSha256,
                                                 0,
                                                 0,
                                                 writeStage);
    }
    return MigrationInstallResult::WRITES_DISABLED;
#endif
}

MigrationInstallResult migrationRestoreSmartPidApp1(const MigrationInstallRequest& request,
                                                    TelemetryPublisher* telemetry) {
    if (!validPackageUrl(request.packageUrl)) {
        if (telemetry) {
            telemetry->publishMigrationInstallStatus("smartpid_oem_app1",
                                                     "rejected",
                                                     "invalid_package_url",
                                                     request.packageUrl,
                                                     request.packageSha256);
        }
        return MigrationInstallResult::INVALID_REQUEST;
    }
    if (!validSha256Hex(request.packageSha256)) {
        if (telemetry) {
            telemetry->publishMigrationInstallStatus("smartpid_oem_app1",
                                                     "rejected",
                                                     "invalid_package_sha256",
                                                     request.packageUrl,
                                                     request.packageSha256);
        }
        return MigrationInstallResult::INVALID_REQUEST;
    }
    if (!runningFromOemLayoutApp0()) {
        if (telemetry) {
            telemetry->publishMigrationInstallStatus("smartpid_oem_app1",
                                                     "rejected",
                                                     "not_running_from_oem_app0",
                                                     request.packageUrl,
                                                     request.packageSha256);
        }
        return MigrationInstallResult::UNSAFE_STATE;
    }

#ifdef DESKTOP_BUILD
    return MigrationInstallResult::UNSAFE_STATE;
#else
    MigrationInstallResult validation = validatePackageStream(request, telemetry, false, false);
    if (validation != MigrationInstallResult::ACCEPTED) return validation;

#if defined(PROOFPRO_ENABLE_OEM_APP_RESTORE)
    MigrationInstallResult writeResult = validatePackageStream(request, telemetry, false, false, true);
    if (writeResult != MigrationInstallResult::ACCEPTED) return writeResult;
    if (telemetry) {
        telemetry->publishMigrationInstallStatus("smartpid_oem_app1",
                                                 "verified",
                                                 "smartpid_oem_app1_written",
                                                 request.packageUrl,
                                                 request.packageSha256,
                                                 0,
                                                 0,
                                                 "smartpid_app1");
    }
    return MigrationInstallResult::ACCEPTED;
#else
    if (telemetry) {
        telemetry->publishMigrationInstallStatus("smartpid_oem_app1",
                                                 "rejected",
                                                 "writes_not_enabled",
                                                 request.packageUrl,
                                                 request.packageSha256,
                                                 0,
                                                 0,
                                                 "smartpid_app1");
    }
    return MigrationInstallResult::WRITES_DISABLED;
#endif
#endif
}
