#ifndef _INCLUDES_H_
#define _INCLUDES_H_
#include "includes.h"
#include "list_utils.h"
#include "timing_functions.h"
#include "GPIO_handle.h"
#endif

#define ESPNOW_MAXDELAY 512

// log TAGs
static const char *TAG0 = "main   ";
static const char *TAG1 = "ND task";
static const char *TAG2 = "timer  ";

// queue handles
static QueueHandle_t s_example_espnow_queue;        // espnow event queue
static QueueHandle_t s_time_send_placed_queue;      // queue for timestamps when espnow_send was called

//timer handles
esp_timer_handle_t msg_exchange_timer_handle;
esp_timer_handle_t shutdown_timer_handle;
esp_timer_handle_t init_deepsleep_timer_handle;

static uint8_t s_example_broadcast_mac[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint16_t s_example_espnow_seq[EXAMPLE_ESPNOW_DATA_MAX] = { 0, 0 };

static void example_espnow_deinit(example_espnow_send_param_t *send_param);

static void example_wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(ESPNOW_WIFI_MODE) );
    ESP_ERROR_CHECK( esp_wifi_start());
    ESP_ERROR_CHECK( esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

#if CONFIG_ESPNOW_ENABLE_LONG_RANGE
    ESP_ERROR_CHECK( esp_wifi_set_protocol(ESPNOW_WIFI_IF, WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G|WIFI_PROTOCOL_11N|WIFI_PROTOCOL_LR) );
#endif
}

static void IRAM_ATTR example_espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    int64_t time_now = get_systime_us();

    example_espnow_event_t evt;
    example_espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
    evt.timestamp = time_now;

    // compute send offset here
    int64_t time_placed;
    if (xQueueReceive(s_time_send_placed_queue, &time_placed, 0) != pdTRUE){
        ESP_LOGW(TAG1, "Receive time_send_placed_queue queue fail");
        send_cb->send_offset = 0;
    }else{
        send_cb->send_offset = time_now - time_placed;
    }

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

static void IRAM_ATTR example_espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    int64_t time_now = get_systime_us();
    
    example_espnow_event_t evt;
    example_espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
    recv_cb->sig_len = recv_info->rx_ctrl->sig_len;

    evt.timestamp = time_now;

    uint8_t * mac_addr = recv_info->src_addr;

    if (mac_addr == NULL || data == NULL || len <= 0) {
        ESP_LOGE(TAG1, "Receive cb arg error");
        return;
    }

    evt.id = EXAMPLE_ESPNOW_RECV_CB;
    memcpy(recv_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    recv_cb->data = (uint8_t*) malloc(len);
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

static void msg_exchange_timer_cb(void* arg){
    // delete timer that called this cb
    ESP_ERROR_CHECK(esp_timer_delete(msg_exchange_timer_handle));
    
    // assert debug pin, start timer for when to init shutdown and display status 
    ESP_ERROR_CHECK( gpio_set_level(GPIO_DEBUG_PIN, 1) );
    ESP_ERROR_CHECK( esp_timer_start_once(shutdown_timer_handle, CONFIG_MSG_EXCHANGE_DURATION_MS*1000) );
    ESP_LOGI(TAG2, "Msg exchange timer triggered - beginning message exchange");
    // insert vTaskCreate() for message exchange with ONC here
}

static void shutdown_timer_cb(void* arg){
    // delete timer that called this cb
    ESP_ERROR_CHECK(esp_timer_delete(shutdown_timer_handle));

    // set timer when to go to sleep
    // maybe create a timekeeper task which supervises the cleanup
    ESP_ERROR_CHECK( esp_timer_start_once(init_deepsleep_timer_handle, 100) );     
    
    ESP_LOGW(TAG2, "Shutdown timer triggered - finishing tasks");
}

static void init_deepsleep_timer_cb(void* arg){
    // delete timer that called this cb
    ESP_ERROR_CHECK(esp_timer_delete(init_deepsleep_timer_handle));
    
    ESP_LOGI(TAG2, "Going into deepsleep for %d", CONFIG_DEEPSLEEP_DURATION_MS);
    
    // de-assert debug pin and enter deepsleep
    ESP_ERROR_CHECK( gpio_set_level(GPIO_DEBUG_PIN, 1) );
    esp_deep_sleep(CONFIG_DEEPSLEEP_DURATION_MS*1000);
}

int example_espnow_data_parse(uint8_t *data, uint16_t data_len, uint16_t *seq)
{
    example_espnow_data_t *buf = (example_espnow_data_t *)data;
    //uint16_t crc, crc_cal = 0;

    if (data_len < sizeof(example_espnow_data_t)) {
        ESP_LOGE(TAG1, "Receive ESPNOW data too short, len:%d", data_len);
        return -1;
    }

    *seq = buf->seq_num;
    /*crc = buf->crc;
    buf->crc = 0;
    crc_cal = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, data_len);

    if (crc_cal == crc) {
        return buf->type;
    }
    */
    return buf->type;
}

void IRAM_ATTR example_espnow_data_prepare(example_espnow_send_param_t *send_param)
{
    example_espnow_data_t *buf = (example_espnow_data_t *)send_param->buffer;

    assert(send_param->len >= sizeof(example_espnow_data_t));

    buf->type = IS_BROADCAST_ADDR(send_param->dest_mac) ? EXAMPLE_ESPNOW_DATA_BROADCAST : EXAMPLE_ESPNOW_DATA_UNICAST;
    buf->seq_num = s_example_espnow_seq[buf->type]++;
    //buf->crc = 0;

    esp_fill_random(buf->payload, send_param->len - sizeof(example_espnow_data_t));
    buf->timestamp = get_systime_us();
    /* buf->crc = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, send_param->len); */
}

static void neighbor_detection_task(void *pvParameter){

    example_espnow_event_t evt;
    uint16_t recv_seq = 0;
    bool is_broadcast = false;
    int ret;
    
    TickType_t wait_duration = portMAX_DELAY;
    int64_t send_offset_sum = 0;
    PeerListHandle_t* peer_list = xInitPeerList();

    ESP_LOGI(TAG1, "Starting broadcast discovery of peers");

    // init peer list
    example_espnow_send_param_t *send_param = (example_espnow_send_param_t *)pvParameter;
    
    // post timestamp of calling esp_now_send() to queue
    int64_t time_placed = get_systime_us(); 
    if (xQueueSend(s_time_send_placed_queue, &time_placed, 0) != pdTRUE){
        ESP_LOGE(TAG1, "Could not post to s_time_send_placed_queue");
        example_espnow_deinit(send_param);
        vTaskDelete(NULL);
    }
    // send first broadcast
    if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK) {
        ESP_LOGE(TAG1, "Send error");
        example_espnow_deinit(send_param);
        vTaskDelete(NULL);
    }

    // start event handling
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

                // delay before sending next data
                if (send_param->delay > 0) {
                    vTaskDelay(send_param->delay/portTICK_PERIOD_MS);
                }

                send_offset_sum += send_cb->send_offset;

                send_param->count--;
                if (send_param->count > 0) {    	    // send next bc if still got some to send

                    //ESP_LOGI(TAG1, "send data to "MACSTR"", MAC2STR(send_cb->mac_addr));

                    //memcpy(send_param->dest_mac, send_cb->mac_addr, ESP_NOW_ETH_ALEN);
                    
                    
                    // post timestamp of calling esp_now_send() to queue 
                    time_placed = get_systime_us();
                    if (xQueueSend(s_time_send_placed_queue, &time_placed, 0) != pdTRUE){
                        ESP_LOGE(TAG1, "Could not post to s_time_send_placed_queue");
                        example_espnow_deinit(send_param);
                        vTaskDelete(NULL);
                    }
                    example_espnow_data_prepare(send_param);
                    // send next broadcast
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
                
                // compute transmission time and offset 
                int64_t time_TX = ((example_espnow_data_t*)(recv_cb->data))->timestamp;
                int64_t time_RX = evt.timestamp;
                const int64_t time_tm = ( recv_cb->sig_len * 8 ); // rate * 10^6 = 1us [in us]
                int64_t delta = time_TX - time_RX - time_tm;

                ret = example_espnow_data_parse(recv_cb->data, recv_cb->data_len, &recv_seq);
                free(recv_cb->data);
                
                if (ret == EXAMPLE_ESPNOW_DATA_BROADCAST) {
                    //ESP_LOGI(TAG1, "Receive %dth broadcast data from: "MACSTR", len: %d", recv_seq, MAC2STR(recv_cb->mac_addr), recv_cb->data_len);

                    if (esp_now_is_peer_exist(recv_cb->mac_addr) == false) {        // unknown peer
                        
                        vAddPeer(peer_list, recv_cb->mac_addr, delta);

                    }else{                                                          // known peer
                        
                        vAddOffset(peer_list, recv_cb->mac_addr, delta);
                    
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

    ESP_LOGW(TAG0, "finished broadcasting at %lld us", esp_timer_get_time());

    // display peer counts and total number of received broadcasts
    ESP_LOGW(TAG1, "Timestamps received from %d peers.", peer_list->peer_count);

    // compute the experienced average send offset
    int64_t avg_send_offset = send_offset_sum / ( CONFIG_ESPNOW_SEND_COUNT-send_param->count ) ;
    ESP_LOGW(TAG1, "avg_send_offset = %lld us", avg_send_offset);

    // determine time master and coresponding address
    int64_t max_offset = 0;
    uint8_t max_offset_addr[ESP_NOW_ETH_ALEN];
    
    for (uint8_t i = 0; i < ESP_NOW_ETH_ALEN; i++){         // copy mac address with highest offset from list
        max_offset_addr[i] = peer_list->max_offset_addr[i];
    }

    // check if any peers have been detected
    if (peer_list->peer_count != 0){
        max_offset = peer_list->max_offset - avg_send_offset;        // extract max offset, take average send offset intop account
    }else{
        ESP_LOGE(TAG1, "No peers detected");
    }
    
    // delete peer list
    vDeletePeerList(peer_list); 
    
    if (max_offset > 0) {                                       // device does not hold the master time 
        ESP_LOGW(TAG1, "Master offset of %lld us by "MACSTR"", max_offset, MAC2STR(max_offset_addr));
    }else{                                                      // this device holds the master time
        esp_err_t err = esp_read_mac(max_offset_addr, ESP_MAC_WIFI_SOFTAP);
        if(err != ESP_OK){
            ESP_LOGE(TAG1, "Error reading my mac address.");    
        }else{
            ESP_LOGW(TAG1, "I have the master time with "MACSTR"", MAC2STR(max_offset_addr));
        }
    }

    // compute delay to start msg exchange in sync with the master time (oldest node);
    int64_t time_now = get_systime_us();
    uint64_t master_time_us = (max_offset > 0) ? (time_now + max_offset) : time_now;

    // find the delay to the next full 100th of a second (of master if not master)
    uint64_t delay_us = (master_time_us / 100000 + 1) * 100000 - master_time_us;
    
    // start scheduled phases according to master time
    ESP_ERROR_CHECK(esp_timer_start_once(msg_exchange_timer_handle, delay_us)); // set timer for when to start masg exchange
    vTaskDelete(NULL);
}

/* 
static void pulse_task(void *pvParameter){

}
*/

static esp_err_t example_espnow_init(void)
{
    example_espnow_send_param_t *send_param;

    s_time_send_placed_queue = xQueueCreate(1, sizeof(int64_t));
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
    esp_now_peer_info_t *peer = (esp_now_peer_info_t*) malloc(sizeof(esp_now_peer_info_t));
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
    send_param->buffer = (uint8_t*)malloc(CONFIG_ESPNOW_SEND_LEN);
    if (send_param->buffer == NULL) {
        ESP_LOGE(TAG1, "Malloc send buffer fail");
        free(send_param);
        vSemaphoreDelete(s_example_espnow_queue);
        esp_now_deinit();
        return ESP_FAIL;
    }
    memcpy(send_param->dest_mac, s_example_broadcast_mac, ESP_NOW_ETH_ALEN);
    example_espnow_data_prepare(send_param);

    xTaskCreate(neighbor_detection_task, "neighbor_detection_task", 2048, send_param, 4, NULL);

    return ESP_OK;
}

static void setup_timer(){
    
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

    ESP_LOGW(TAG0, "Entering app_main() at %lld us", esp_timer_get_time());

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

    setup_GPIO();
    setup_timer();
    example_wifi_init();
    example_espnow_init();
}
