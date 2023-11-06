
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
//#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_crc.h"
#include "esp_sleep.h"
#include "esp_timer.h"
//#include "timer_config.h"
#include "timing_functions.h"
#include "espnow_example.h"

#define ESPNOW_MAXDELAY 512

// log TAGs

static const char *TAG1 = "neighbor detection task";
static const char *TAG2 = "systime";
static const char *TAG3 = "timer";

// queue handles
static QueueHandle_t s_example_espnow_queue;
//static QueueHandle_t s_time_offset_queue;

//timer handles
esp_timer_handle_t msg_exchange_timer_handle;
esp_timer_handle_t shutdown_timer_handle;
esp_timer_handle_t init_deepsleep_timer_handle;

static uint8_t s_example_broadcast_mac[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint16_t s_example_espnow_seq[EXAMPLE_ESPNOW_DATA_MAX] = { 0, 0 };

static void example_espnow_deinit(example_espnow_send_param_t *send_param);

static void msg_exchange_timer_cb(void);
static void shutdown_timer_cb(void);
static void init_deepsleep_timer_cb(void);


static void example_wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(ESPNOW_WIFI_MODE) );
    //ESP_ERROR_CHECK( esp_wifi_set_ps(WIFI_PS_NONE));            // timestamps funktionieren sonst nicht
    ESP_ERROR_CHECK( esp_wifi_start());
    ESP_ERROR_CHECK( esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

#if CONFIG_ESPNOW_ENABLE_LONG_RANGE
    ESP_ERROR_CHECK( esp_wifi_set_protocol(ESPNOW_WIFI_IF, WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G|WIFI_PROTOCOL_11N|WIFI_PROTOCOL_LR) );
#endif
}

static void example_espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    int64_t time_now = get_systime_us();

    // compute send offset here
    
    example_espnow_event_t evt;
    example_espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
    evt.timestamp = time_now;

    if (mac_addr == NULL) {
        ESP_LOGE(TAG1, "Send cb arg error");
        return;
    }

    evt.id = EXAMPLE_ESPNOW_SEND_CB;
    memcpy(send_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    send_cb->status = status;
    if (xQueueSend(s_example_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
        ESP_LOGW(TAG1, "Send send queue fail");
    }
}

static void example_espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    example_espnow_event_t evt;
    example_espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
    recv_cb->sig_len = recv_info->rx_ctrl->sig_len;
    //evt.timestamp = (int64_t)(recv_info->rx_ctrl->timestamp);
    evt.timestamp = get_systime_us();

    uint8_t * mac_addr = recv_info->src_addr;

    if (mac_addr == NULL || data == NULL || len <= 0) {
        ESP_LOGE(TAG1, "Receive cb arg error");
        return;
    }

    evt.id = EXAMPLE_ESPNOW_RECV_CB;
    memcpy(recv_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    recv_cb->data = malloc(len);
    if (recv_cb->data == NULL) {
        ESP_LOGE(TAG1, "Malloc receive data fail");
        return;
    }
    memcpy(recv_cb->data, data, len);
    recv_cb->data_len = len;
    if (xQueueSend(s_example_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
        ESP_LOGW(TAG1, "Send receive queue fail");
        free(recv_cb->data);
    }
}

static void msg_exchange_timer_cb(void){
    esp_timer_start_once(shutdown_timer_handle, CONFIG_MSG_EXCHANGE_DURATION_MS*1000);

    // delete timer that called this cb
    ESP_ERROR_CHECK(esp_timer_stop(msg_exchange_timer_handle));
    ESP_ERROR_CHECK(esp_timer_delete(msg_exchange_timer_handle));
    ESP_LOGI(TAG3, "Msg exchange timer triggered - beginning message exchange");

    // insert vTaskCreate() for message exchange with ONC here
}

static void shutdown_timer_cb(void){
    esp_timer_enable_once(init_deepsleep_timer_handle, 100000);     // set init deepsleep timer
    // delete timer that called this cb
    ESP_ERROR_CHECK(esp_timer_stop(shutdown_timer_handle));
    ESP_ERROR_CHECK(esp_timer_delete(shutdown_timer_handle));
    ESP_LOGW(TAG3, "Shutdown timer triggered - finishing tasks");

    // maybe create a timekeeper task which supervises the cleanup
}

static void init_deepsleep_timer_cb(void){
    // delete timer that called this cb
    ESP_ERROR_CHECK(esp_timer_stop(init_deepsleep_timer_handle));
    ESP_ERROR_CHECK(esp_timer_delete(init_deepsleep_timer_handle));
    
    ESP_LOGI(TAG3, "Going into deepsleep for %d", #CONFIG_DEEPSLEEP_DURATION_MS);

    // init deepsleep
    ESP_ERROR_CHECK(esp_deep_sleep(CONFIG_DEEPSLEEP_DURATION_MS*1000));
}

int example_espnow_data_parse(uint8_t *data, uint16_t data_len, uint16_t *seq)
{
    example_espnow_data_t *buf = (example_espnow_data_t *)data;
    uint16_t crc, crc_cal = 0;

    if (data_len < sizeof(example_espnow_data_t)) {
        ESP_LOGE(TAG1, "Receive ESPNOW data too short, len:%d", data_len);
        return -1;
    }

    *seq = buf->seq_num;
    crc = buf->crc;
    buf->crc = 0;
    crc_cal = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, data_len);

    if (crc_cal == crc) {
        return buf->type;
    }

    return -1;
}

void example_espnow_data_prepare(example_espnow_send_param_t *send_param)
{
    example_espnow_data_t *buf = (example_espnow_data_t *)send_param->buffer;

    assert(send_param->len >= sizeof(example_espnow_data_t));

    buf->type = IS_BROADCAST_ADDR(send_param->dest_mac) ? EXAMPLE_ESPNOW_DATA_BROADCAST : EXAMPLE_ESPNOW_DATA_UNICAST;
    buf->seq_num = s_example_espnow_seq[buf->type]++;
    buf->crc = 0;
    buf->timestamp = get_systime_us();

    /* Fill all remaining bytes after the timestamp with random values */
    esp_fill_random(buf->payload, send_param->len - sizeof(example_espnow_data_t));
    buf->crc = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, send_param->len);
}

static void neighbor_detection_task(void *pvParameter){

    example_espnow_event_t evt;
    uint16_t recv_seq = 0;
    bool is_broadcast = false;
    int ret;
    
    TickType_t wait_duration = portMAX_DELAY;
    
    uint8_t peer_count = 0;
    uint8_t exchange_count = 0;
    int64_t time_offset_sum = 0;

    ESP_LOGI(TAG1, "Starting broadcast discovery of peers");

    /* Start sending a broadcast frame with ESPNOW data. */
    example_espnow_send_param_t *send_param = (example_espnow_send_param_t *)pvParameter;
    if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK) {
        ESP_LOGE(TAG1, "Send error");
        example_espnow_deinit(send_param);
        vTaskDelete(NULL);
    }

    while (xQueueReceive(s_example_espnow_queue, &evt, wait_duration) == pdTRUE) {
        switch (evt.id) {
            case EXAMPLE_ESPNOW_SEND_CB:
            {
                example_espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
                is_broadcast = IS_BROADCAST_ADDR(send_cb->mac_addr);

                //ESP_LOGD(TAG1, "Send broadcast to "MACSTR", status1: %d", MAC2STR(send_cb->mac_addr), send_cb->status);

                if (is_broadcast && (send_param->broadcast == false)) {
                    break;
                }

                /* Delay a while before sending the next data. */
                if (send_param->delay > 0) {
                    vTaskDelay(send_param->delay/portTICK_PERIOD_MS);
                }

                send_param->count--;
                if (send_param->count > 0) {    	    // send next bc if still got some to send

                    //ESP_LOGI(TAG1, "send data to "MACSTR"", MAC2STR(send_cb->mac_addr));

                    memcpy(send_param->dest_mac, send_cb->mac_addr, ESP_NOW_ETH_ALEN);
                    example_espnow_data_prepare(send_param);

                    /* Send the next data after the previous data is sent. */
                    if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK) {
                        ESP_LOGE(TAG1, "Send error");
                        example_espnow_deinit(send_param);
                        vTaskDelete(NULL);
                    }

                }else {
                    ESP_LOGW(TAG1, "last broadcast sent");
                    wait_duration = send_param->delay * 2;
                }

                break;
            }
            case EXAMPLE_ESPNOW_RECV_CB:
            {
                example_espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
                
                // calculate RTC offset and correct the systime 
                int64_t time_TX = ((example_espnow_data_t*)(recv_cb->data))->timestamp;
                int64_t time_RX = evt.timestamp;

                const int64_t time_tm = ( recv_cb->sig_len * 8 ); // rate * 10^6 = 1us [in us]

                //ESP_LOGW(TAG2, "time_tm = %lld", time_tm);

                int64_t delta = time_TX - time_RX - time_tm;
                exchange_count++;
                time_offset_sum += delta;

                //ESP_LOGW(TAG2, "time_TX = %lld | time_RX = %lld | time_tm = %lld | delta = %lld us | count = %u ", time_TX, time_RX, time_tm, delta, exchange_count);

                ret = example_espnow_data_parse(recv_cb->data, recv_cb->data_len, &recv_seq);
                free(recv_cb->data);
                
                if (ret == EXAMPLE_ESPNOW_DATA_BROADCAST) {
                    //ESP_LOGI(TAG1, "Receive %dth broadcast data from: "MACSTR", len: %d", recv_seq, MAC2STR(recv_cb->mac_addr), recv_cb->data_len);

                    /* If MAC address does not exist in peer list, add it to peer list. */
                    if (esp_now_is_peer_exist(recv_cb->mac_addr) == false) {
                        peer_count++;
                        esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
                        if (peer == NULL) {
                            ESP_LOGE(TAG1, "Malloc peer information fail");
                            example_espnow_deinit(send_param);
                            vTaskDelete(NULL);
                        }
                        memset(peer, 0, sizeof(esp_now_peer_info_t));
                        peer->channel = CONFIG_ESPNOW_CHANNEL;
                        peer->ifidx = ESPNOW_WIFI_IF;
                        peer->encrypt = true;
                        memcpy(peer->lmk, CONFIG_ESPNOW_LMK, ESP_NOW_KEY_LEN);
                        memcpy(peer->peer_addr, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
                        ESP_ERROR_CHECK( esp_now_add_peer(peer) );
                        free(peer);
                    }
                }
                else if (ret == EXAMPLE_ESPNOW_DATA_UNICAST) {
                    ESP_LOGE(TAG1, "Received unicast data : "MACSTR"", MAC2STR(recv_cb->mac_addr));
                }
                else {
                    ESP_LOGE(TAG1, "Receive error data from: "MACSTR"", MAC2STR(recv_cb->mac_addr));
                }
                break;
            }
        }
    }

    ESP_LOGW(TAG2, "A total of %u timing exchanges from %d peers took place", exchange_count, peer_count);

    int64_t avg_time_offset_us = 0;
    if (exchange_count > 0){
        avg_time_offset_us = time_offset_sum / (int64_t)exchange_count;
        ESP_LOGW(TAG2, "correct time offset of: %lld us", avg_time_offset_us);
        correct_systime(avg_time_offset_us);
    }else{
        ESP_LOGE(TAG2, "No time exchanges took place");
    }
    
    uint64_t delay_us = 0;
    uint64_t time_subsec_us = get_systime_us() % 1000000;     // time under a second

    // set timer on next full millisecond
    for(uint8_t i = 1; i <= 10; i++){
        if(time_subsec_us < 100000*i){
            delay_us = 100000*i - time_subsec_us;   // compute time to next tenth of a second
            break;
        }
    }

    ESP_ERROR_CHECK(esp_timer_start_once(msg_exchange_timer_handle, delay_us)); // set timer on next tenth of a second
}

static esp_err_t example_espnow_init(void)
{
    example_espnow_send_param_t *send_param;

    s_example_espnow_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(example_espnow_event_t));
    if (s_example_espnow_queue == NULL) {
        ESP_LOGE(TAG1, "Create mutex fail");
        return ESP_FAIL;
    }

    /* Initialize ESPNOW and register sending and receiving callback function. */
    ESP_ERROR_CHECK( esp_now_init() );
    ESP_ERROR_CHECK( esp_now_register_send_cb(example_espnow_send_cb) );
    ESP_ERROR_CHECK( esp_now_register_recv_cb(example_espnow_recv_cb) );
#if CONFIG_ESPNOW_ENABLE_POWER_SAVE
    ESP_ERROR_CHECK( esp_now_set_wake_window(CONFIG_ESPNOW_WAKE_WINDOW) );
    ESP_ERROR_CHECK( esp_wifi_connectionless_module_set_wake_interval(CONFIG_ESPNOW_WAKE_INTERVAL) );
#endif
    /* Set primary master key. */
    ESP_ERROR_CHECK( esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK) );

    /* Add broadcast peer information to peer list. */
    esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
    if (peer == NULL) {
        ESP_LOGE(TAG1, "Malloc peer information fail");
        vSemaphoreDelete(s_example_espnow_queue);
        esp_now_deinit();
        return ESP_FAIL;
    }
    memset(peer, 0, sizeof(esp_now_peer_info_t));
    peer->channel = CONFIG_ESPNOW_CHANNEL;
    peer->ifidx = ESPNOW_WIFI_IF;
    peer->encrypt = false;
    memcpy(peer->peer_addr, s_example_broadcast_mac, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK( esp_now_add_peer(peer) );
    free(peer);

    /* Initialize sending parameters. */
    send_param = malloc(sizeof(example_espnow_send_param_t));
    if (send_param == NULL) {
        ESP_LOGE(TAG1, "Malloc send parameter fail");
        vSemaphoreDelete(s_example_espnow_queue);
        esp_now_deinit();
        return ESP_FAIL;
    }
    memset(send_param, 0, sizeof(example_espnow_send_param_t));
    send_param->unicast = false;
    send_param->broadcast = true;
    send_param->count = CONFIG_ESPNOW_SEND_COUNT;
    send_param->delay = CONFIG_ESPNOW_SEND_DELAY;
    send_param->len = CONFIG_ESPNOW_SEND_LEN;
    send_param->buffer = malloc(CONFIG_ESPNOW_SEND_LEN);
    if (send_param->buffer == NULL) {
        ESP_LOGE(TAG1, "Malloc send buffer fail");
        free(send_param);
        vSemaphoreDelete(s_example_espnow_queue);
        esp_now_deinit();
        return ESP_FAIL;
    }
    memcpy(send_param->dest_mac, s_example_broadcast_mac, ESP_NOW_ETH_ALEN);
    example_espnow_data_prepare(send_param);

    xTaskCreate(neighbor_detection_task, "neighbor_detection_task", 4096, send_param, 4, NULL);

    return ESP_OK;
}

static esp_err_t setup_timer(){
    
    // timer zum starten der mesaage exchange phase
    const esp_timer_create_args_t msg_exchange_timer_args = {
            .callback = &msg_exchange_timer_cb,
            .name = "msg_exchange_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&msg_exchange_timer_args, &msg_exchange_timer_handle));

    // timer zum beenden aller tasks
    const esp_timer_create_args_t shutdown_timer_args = {
            .callback = &shutdown_timer_cb,
            .name = "shutdown_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&shutdown_timer_args, &shutdown_timer_handle));

    // timer zum initialisieren des deepsleeps
    const esp_timer_create_args_t init_deepsleep_timer_args = {
            .callback = &init_deepsleep_timer_cb,
            .name = "init_deepsleep_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&init_deepsleep_timer_args, &init_deepsleep_timer_handle));
}

static void example_espnow_deinit(example_espnow_send_param_t *send_param)
{
    free(send_param->buffer);
    free(send_param);
    vSemaphoreDelete(s_example_espnow_queue);
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();
}

void app_main(void)
{
    ESP_LOGW(TAG3, "Entering app_main() at %lld us", esp_timer_get_time());
    
    // reset systime if reset did not get triggered by deepsleep
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP){
        ESP_LOGE(TAG1, "Resetting systime - not booting from deepsleep");
        reset_systime();
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    timer_setup();
    example_wifi_init();
    example_espnow_init();
}
