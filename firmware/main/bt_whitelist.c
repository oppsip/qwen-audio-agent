/**
 * Phone-number whitelist sourced from the connected phone's contacts (PBAP).
 * Used only in HFP Client mode: matching CLIP skips SIP forward and BT audio.
 */

#include "bt_whitelist.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "bt_wl";

#define NVS_NS_WL   "wl"
#define NVS_KEY_N   "count"
#define NVS_KEY_L   "list"

static bt_wl_contact_t s_list[BT_WL_MAX];
static int s_count;
static SemaphoreHandle_t s_mux;

static void lock(void)
{
    if (s_mux) {
        xSemaphoreTake(s_mux, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_mux) {
        xSemaphoreGive(s_mux);
    }
}

static void digits_only(const char *in, char *out, size_t out_sz)
{
    size_t n = 0;
    if (out_sz == 0) {
        return;
    }
    if (in) {
        for (const char *p = in; *p && n + 1 < out_sz; p++) {
            if (*p >= '0' && *p <= '9') {
                out[n++] = *p;
            }
        }
    }
    out[n] = '\0';
}

static bool numbers_match(const char *a, const char *b)
{
    char da[BT_WL_NUM_LEN * 2];
    char db[BT_WL_NUM_LEN * 2];
    digits_only(a, da, sizeof(da));
    digits_only(b, db, sizeof(db));
    size_t la = strlen(da);
    size_t lb = strlen(db);
    if (la == 0 || lb == 0) {
        return false;
    }
    if (strcmp(da, db) == 0) {
        return true;
    }
    size_t n = la < lb ? la : lb;
    if (n > 11) {
        n = 11;
    }
    if (n < 7) {
        return false;
    }
    return strcmp(da + la - n, db + lb - n) == 0;
}

static void persist_locked(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WL, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed");
        return;
    }
    nvs_set_u16(h, NVS_KEY_N, (uint16_t)s_count);
    if (s_count > 0) {
        nvs_set_blob(h, NVS_KEY_L, s_list, (size_t)s_count * sizeof(s_list[0]));
    } else {
        nvs_erase_key(h, NVS_KEY_L);
    }
    nvs_commit(h);
    nvs_close(h);
}

static void load_from_nvs(void)
{
    nvs_handle_t h;
    s_count = 0;
    memset(s_list, 0, sizeof(s_list));
    if (nvs_open(NVS_NS_WL, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint16_t n = 0;
    if (nvs_get_u16(h, NVS_KEY_N, &n) != ESP_OK || n == 0) {
        nvs_close(h);
        return;
    }
    if (n > BT_WL_MAX) {
        n = BT_WL_MAX;
    }
    size_t sz = (size_t)n * sizeof(s_list[0]);
    if (nvs_get_blob(h, NVS_KEY_L, s_list, &sz) == ESP_OK) {
        s_count = (int)(sz / sizeof(s_list[0]));
        if (s_count > BT_WL_MAX) {
            s_count = BT_WL_MAX;
        }
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Loaded %d contact(s)", s_count);
}

static void trim_copy(char *dst, size_t dst_sz, const char *src)
{
    if (dst_sz == 0) {
        return;
    }
    while (src && (*src == ' ' || *src == '\t' || *src == '"')) {
        src++;
    }
    size_t n = src ? strlen(src) : 0;
    while (n > 0 && (src[n - 1] == ' ' || src[n - 1] == '\t' ||
                     src[n - 1] == '\r' || src[n - 1] == '"' || src[n - 1] == ';')) {
        n--;
    }
    if (n >= dst_sz) {
        n = dst_sz - 1;
    }
    if (src && n) {
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

static const char *vcard_value(const char *line)
{
    const char *p = strchr(line, ':');
    return p ? p + 1 : "";
}

static bool prop_is(const char *line, const char *name)
{
    size_t n = strlen(name);
    if (strlen(line) < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char a = line[i];
        char b = name[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    char c = line[n];
    return c == ':' || c == ';' || c == '.';
}

static void name_from_n(char *dst, size_t dst_sz, const char *n_val)
{
    char last[BT_WL_NAME_LEN] = {0};
    char first[BT_WL_NAME_LEN] = {0};
    const char *semi = strchr(n_val, ';');
    if (semi) {
        size_t ln = (size_t)(semi - n_val);
        if (ln >= sizeof(last)) {
            ln = sizeof(last) - 1;
        }
        memcpy(last, n_val, ln);
        last[ln] = '\0';
        trim_copy(first, sizeof(first), semi + 1);
        char *semi2 = strchr(first, ';');
        if (semi2) {
            *semi2 = '\0';
        }
    } else {
        trim_copy(last, sizeof(last), n_val);
    }
    if (first[0] && last[0]) {
        snprintf(dst, dst_sz, "%s %s", first, last);
    } else if (first[0]) {
        trim_copy(dst, dst_sz, first);
    } else {
        trim_copy(dst, dst_sz, last);
    }
}

static bool add_locked(const char *name, const char *number)
{
    char num[BT_WL_NUM_LEN];
    trim_copy(num, sizeof(num), number);
    char dig[BT_WL_NUM_LEN * 2];
    digits_only(num, dig, sizeof(dig));
    if (dig[0] == '\0') {
        return false;
    }
    for (int i = 0; i < s_count; i++) {
        if (numbers_match(s_list[i].number, num)) {
            if (name && name[0] && s_list[i].name[0] == '\0') {
                trim_copy(s_list[i].name, sizeof(s_list[i].name), name);
            }
            return false;
        }
    }
    if (s_count >= BT_WL_MAX) {
        return false;
    }
    memset(&s_list[s_count], 0, sizeof(s_list[s_count]));
    trim_copy(s_list[s_count].number, sizeof(s_list[s_count].number), num);
    if (name && name[0]) {
        trim_copy(s_list[s_count].name, sizeof(s_list[s_count].name), name);
    }
    s_count++;
    return true;
}

static void parse_vcard_block(const char *block)
{
    char fn[BT_WL_NAME_LEN] = {0};
    char n_name[BT_WL_NAME_LEN] = {0};
    char tels[8][BT_WL_NUM_LEN];
    int ntel = 0;
    memset(tels, 0, sizeof(tels));

    const char *p = block;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t llen = nl ? (size_t)(nl - p) : strlen(p);
        char line[160];
        if (llen >= sizeof(line)) {
            llen = sizeof(line) - 1;
        }
        memcpy(line, p, llen);
        line[llen] = '\0';
        if (llen > 0 && line[llen - 1] == '\r') {
            line[llen - 1] = '\0';
        }
        if (prop_is(line, "FN") && fn[0] == '\0') {
            trim_copy(fn, sizeof(fn), vcard_value(line));
        } else if (prop_is(line, "N") && n_name[0] == '\0') {
            name_from_n(n_name, sizeof(n_name), vcard_value(line));
        } else if (prop_is(line, "TEL") && ntel < 8) {
            trim_copy(tels[ntel], sizeof(tels[ntel]), vcard_value(line));
            if (tels[ntel][0]) {
                ntel++;
            }
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }

    const char *name = fn[0] ? fn : n_name;
    for (int i = 0; i < ntel; i++) {
        add_locked(name, tels[i]);
    }
}

void bt_whitelist_init(void)
{
    if (s_mux == NULL) {
        s_mux = xSemaphoreCreateMutex();
    }
    lock();
    load_from_nvs();
    unlock();
}

void bt_whitelist_clear(void)
{
    lock();
    s_count = 0;
    memset(s_list, 0, sizeof(s_list));
    nvs_handle_t h;
    if (nvs_open(NVS_NS_WL, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    unlock();
}

bool bt_whitelist_contains(const char *number)
{
    if (number == NULL || number[0] == '\0') {
        return false;
    }
    bool hit = false;
    lock();
    for (int i = 0; i < s_count; i++) {
        if (numbers_match(s_list[i].number, number)) {
            hit = true;
            break;
        }
    }
    unlock();
    return hit;
}

int bt_whitelist_copy(bt_wl_contact_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    lock();
    int n = s_count;
    if (n > max) {
        n = max;
    }
    memcpy(out, s_list, (size_t)n * sizeof(s_list[0]));
    unlock();
    return n;
}

int bt_whitelist_count(void)
{
    lock();
    int n = s_count;
    unlock();
    return n;
}

esp_err_t bt_whitelist_import_vcf(const char *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Work on a NUL-terminated copy in PSRAM so we can walk with strchr. */
    char *buf = heap_caps_malloc(len + 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = malloc(len + 2);
    }
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf, data, len);
    buf[len] = '\n';
    buf[len + 1] = '\0';

    lock();
    s_count = 0;
    memset(s_list, 0, sizeof(s_list));

    char *p = buf;
    while (p && *p) {
        char *beg = strstr(p, "BEGIN:VCARD");
        if (beg == NULL) {
            beg = strstr(p, "begin:vcard");
        }
        if (beg == NULL) {
            break;
        }
        char *end = strstr(beg, "END:VCARD");
        if (end == NULL) {
            end = strstr(beg, "end:vcard");
        }
        if (end == NULL) {
            break;
        }
        *end = '\0';
        parse_vcard_block(beg);
        p = end + 1;
    }

    persist_locked();
    int n = s_count;
    unlock();
    free(buf);
    ESP_LOGI(TAG, "Imported %d whitelist number(s) from phone book", n);
    return ESP_OK;
}
