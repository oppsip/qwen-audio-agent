#ifndef ETHERNET_MANAGER_H
#define ETHERNET_MANAGER_H

#include "esp_err.h"
#include "esp_netif.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*on_link_up)(void);
    void (*on_connected)(void);
    void (*on_disconnected)(void);
} ethernet_manager_callbacks_t;

esp_err_t ethernet_manager_init(const ethernet_manager_callbacks_t *callbacks);

bool ethernet_manager_is_connected(void);

esp_netif_t *ethernet_manager_netif(void);

#ifdef __cplusplus
}
#endif

#endif /* ETHERNET_MANAGER_H */
