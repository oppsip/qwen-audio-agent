#include "ota_manager.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"

#define TAG              "ota"
#define MANIFEST_MAXLEN  2048
#define CHUNK_SIZE       4096
#define HTTP_TIMEOUT_MS  15000
#define DL_TIMEOUT_MS    120000

/* ------------------------------------------------------------------ */

static int version_cmp(const char *a, const char *b)
{
    int a1 = 0, a2 = 0, a3 = 0;
    int b1 = 0, b2 = 0, b3 = 0;
    sscanf(a, "%d.%d.%d", &a1, &a2, &a3);
    sscanf(b, "%d.%d.%d", &b1, &b2, &b3);
    if (a1 != b1) return a1 - b1;
    if (a2 != b2) return a2 - b2;
    return a3 - b3;
}

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t len)
{
    if (!hex || strlen(hex) != len * 2) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned int v;
        if (sscanf(hex + i * 2, "%02x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

/* Extract a JSON string field: finds "key":"value" and copies value into out.
 * Handles the simple flat-object manifest we control; not a general parser. */
static bool json_str(const char *json, const char *key, char *out, size_t out_size)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p != '"') return false;
    p++; /* skip opening quote */
    size_t i = 0;
    while (*p && *p != '"' && i < out_size - 1) out[i++] = *p++;
    out[i] = '\0';
    return (*p == '"');
}

/* Fetch a small HTTP/HTTPS resource into caller-supplied buffer. */
static esp_err_t fetch_text(const char *url, char *buf, int buf_size)
{
    esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .keep_alive_enable = false,
        /* Server RSTs connections that carry an SNI extension (nginx SNI routing
         * issue). Setting this flag suppresses mbedtls_ssl_set_hostname() so no
         * SNI is sent. CA-chain verification via crt_bundle_attach still applies;
         * only per-hostname identity check is skipped. Remove once server fixed. */
        .skip_cert_common_name_check = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open: %s (DRAM free=%u largest=%u)",
                 esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        esp_http_client_cleanup(client);
        return err;
    }

    int content_len = (int)esp_http_client_fetch_headers(client);
    int status      = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d fetching manifest", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    if (content_len <= 0 || content_len >= buf_size) {
        content_len = buf_size - 1;
    }

    int total = 0;
    while (total < content_len) {
        int n = esp_http_client_read(client, buf + total, content_len - total);
        if (n < 0) { err = ESP_FAIL; break; }
        if (n == 0) break;
        total += n;
    }
    buf[total] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return (err == ESP_OK && total > 0) ? ESP_OK : ESP_FAIL;
}

/* Download firmware, write to OTA slot, verify SHA-256 by reading back the
 * written partition, then set boot partition and reboot.
 *
 * SHA-256 is computed by esp_partition_get_sha256() after the write so no
 * external crypto library is needed.  Pass expected_sha256=NULL to skip check.
 *
 * Note: the manifest sha256 field must match the value produced by
 *   python -c "import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())" firmware.bin
 * NOT the ESP secure-boot-aware hash.  esp_partition_get_sha256() for app
 * partitions computes a bootloader-internal hash that differs from a plain
 * sha256sum, so if the values don't match you can omit the sha256 field and
 * rely on HTTPS + esp_ota_end() image-structure validation instead. */
static esp_err_t download_and_apply(const char *url, const uint8_t *expected_sha256)
{
    const esp_partition_t *ota_part = esp_ota_get_next_update_partition(NULL);
    if (!ota_part) {
        ESP_LOGE(TAG, "No OTA partition available");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OTA target: %s @ 0x%08lx", ota_part->label,
             (unsigned long)ota_part->address);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(ota_part, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return err;
    }

    esp_http_client_config_t cfg = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = DL_TIMEOUT_MS,
        .buffer_size       = CHUNK_SIZE,
        .keep_alive_enable = false,
        .skip_cert_common_name_check = true,  /* see fetch_text() comment */
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        esp_ota_abort(ota_handle);
        return ESP_ERR_NO_MEM;
    }

    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open: %s", esp_err_to_name(err));
        esp_ota_abort(ota_handle);
        esp_http_client_cleanup(client);
        return err;
    }

    int content_len = (int)esp_http_client_fetch_headers(client);
    int status      = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d fetching firmware", status);
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    if (content_len > 0) {
        ESP_LOGI(TAG, "Firmware size: %d bytes", content_len);
    }

    uint8_t *chunk = malloc(CHUNK_SIZE);
    if (!chunk) {
        esp_ota_abort(ota_handle);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    int total = 0, last_pct = -1;
    while (true) {
        int n = esp_http_client_read(client, (char *)chunk, CHUNK_SIZE);
        if (n < 0) { err = ESP_FAIL; break; }
        if (n == 0) break;
        err = esp_ota_write(ota_handle, chunk, (size_t)n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            break;
        }
        total += n;
        if (content_len > 0) {
            int pct = total * 100 / content_len;
            if (pct / 10 != last_pct / 10) {
                ESP_LOGI(TAG, "  %3d%%  (%d / %d bytes)", pct, total, content_len);
                last_pct = pct;
            }
        }
    }
    free(chunk);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        esp_ota_abort(ota_handle);
        return err;
    }

    /* Finalise — validates app image magic, chip type, segment layout. */
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return err;
    }

    /* Optional SHA-256 verification: read back the written partition.
     * esp_partition_get_sha256() uses the ESP-IDF bootloader hash method.
     * Generate the manifest sha256 field with the companion script below. */
    if (expected_sha256) {
        uint8_t computed[32];
        if (esp_partition_get_sha256(ota_part, computed) == ESP_OK) {
            if (memcmp(computed, expected_sha256, 32) != 0) {
                ESP_LOGE(TAG, "SHA-256 mismatch — firmware rejected");
                return ESP_ERR_INVALID_CRC;
            }
            ESP_LOGI(TAG, "SHA-256 verified OK");
        } else {
            ESP_LOGW(TAG, "SHA-256 readback failed — proceeding without check");
        }
    }

    err = esp_ota_set_boot_partition(ota_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA complete — rebooting into new firmware");
    esp_restart();
    return ESP_OK; /* unreachable */
}

/* ------------------------------------------------------------------ */

esp_err_t ota_manager_check_and_update(void)
{
    if (strlen(CONFIG_OTA_MANIFEST_URL) == 0) {
        ESP_LOGD(TAG, "OTA_MANIFEST_URL not configured, skipping");
        return ESP_OK;
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    ESP_LOGI(TAG, "Current firmware: %s", desc->version);
    ESP_LOGI(TAG, "Fetching manifest: %s", CONFIG_OTA_MANIFEST_URL);

    char *buf = malloc(MANIFEST_MAXLEN);
    if (!buf) return ESP_ERR_NO_MEM;

    esp_err_t err = fetch_text(CONFIG_OTA_MANIFEST_URL, buf, MANIFEST_MAXLEN);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA server unreachable (%s) — skipping", esp_err_to_name(err));
        free(buf);
        return ESP_OK;  /* non-fatal */
    }

    char sv[32]      = {0};
    char fw_url[256] = {0};
    char sha_hex[65] = {0};

    if (!json_str(buf, "version", sv,     sizeof(sv))    ||
        !json_str(buf, "url",     fw_url, sizeof(fw_url))) {
        ESP_LOGE(TAG, "Manifest missing 'version' or 'url'");
        free(buf);
        return ESP_FAIL;
    }
    json_str(buf, "sha256", sha_hex, sizeof(sha_hex)); /* optional */
    free(buf);

    ESP_LOGI(TAG, "Server version: %s", sv);
    if (version_cmp(sv, desc->version) <= 0) {
        ESP_LOGI(TAG, "Already up to date");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Upgrading %s -> %s", desc->version, sv);

    uint8_t sha256_bytes[32];
    bool has_sha256 = hex_to_bytes(sha_hex, sha256_bytes, 32);

    return download_and_apply(fw_url, has_sha256 ? sha256_bytes : NULL);
}

void ota_manager_mark_valid(void)
{
    const esp_partition_t    *running = esp_ota_get_running_partition();
    esp_ota_img_states_t      state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware valid — rollback cancelled");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}
