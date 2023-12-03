#ifndef _INCLUDES_H_
#define _INCLUDES_H_
#include "includes.h"
#endif

#define RTC_NAMESPACE "storage"
#define RTC_KEY "value"

void nvs_storeOffset(int64_t value) {
    nvs_handle_t rtc_handle;
    esp_err_t err;

    // open NVS
    err = nvs_open(RTC_NAMESPACE, NVS_READWRITE, &rtc_handle);
    if (err != ESP_OK) {
        printf("Error opening NVS: %s\n", esp_err_to_name(err));
        return;
    }

    // write value to NVS
    err = nvs_set_i64(rtc_handle, RTC_KEY, value);
    if (err != ESP_OK) {
        printf("Error writing to NVS: %s\n", esp_err_to_name(err));
    }

    // commit the changes
    err = nvs_commit(rtc_handle);
    if (err != ESP_OK) {
        printf("Error committing NVS changes: %s\n", esp_err_to_name(err));
    }

    // close NVS
    nvs_close(rtc_handle);
}

uint32_t nvs_loadOffset() {
    nvs_handle_t rtc_handle;
    esp_err_t err;
    int64_t value = 0; // default value if not found

    // open NVS
    err = nvs_open(RTC_NAMESPACE, NVS_READWRITE, &rtc_handle);
    if (err != ESP_OK) {
        printf("Error opening NVS: %s\n", esp_err_to_name(err));
        return value;
    }

    // read value from NVS
    err = nvs_get_i64(rtc_handle, RTC_KEY, &value);
    if (err != ESP_OK) {
        printf("Error reading from NVS: %s\n", esp_err_to_name(err));
    }

    // close NVS
    nvs_close(rtc_handle);

    return value;
}