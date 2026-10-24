#ifndef BT_WHITELIST_H
#define BT_WHITELIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BT_WL_MAX       64
#define BT_WL_NAME_LEN  24
#define BT_WL_NUM_LEN   20

typedef struct {
    char name[BT_WL_NAME_LEN];
    char number[BT_WL_NUM_LEN];
} bt_wl_contact_t;

void bt_whitelist_init(void);

/** Erase stored contacts (factory reset). Enable flag is in the bt namespace. */
void bt_whitelist_clear(void);

bool bt_whitelist_contains(const char *number);

int bt_whitelist_copy(bt_wl_contact_t *out, int max);

int bt_whitelist_count(void);

/** Replace the stored list by parsing a PBAP vCard dump. */
esp_err_t bt_whitelist_import_vcf(const char *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* BT_WHITELIST_H */
