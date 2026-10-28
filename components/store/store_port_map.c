// The nvs_open result → tri-state mapping, shared by the ESP port and the
// host port so the host tests cover the exact mapping the device runs
// (fw#22 review): ESP_OK → FOUND, ESP_ERR_NVS_NOT_FOUND (namespace absent
// on a READONLY open — a blank chip) → ABSENT, anything else → ERROR.
#include "store_port.h"

store_port_nvs_res_t store_port_open_res(int esp_err)
{
    if (esp_err == STORE_PORT_ESP_OK) {
        return STORE_PORT_FOUND;
    }
    if (esp_err == STORE_PORT_ESP_ERR_NVS_NOT_FOUND) {
        return STORE_PORT_ABSENT; // namespace absent ⇒ key absent
    }
    return STORE_PORT_ERROR;
}
