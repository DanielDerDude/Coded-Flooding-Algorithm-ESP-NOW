#ifndef _INCLUDES_H_
#define _INCLUDES_H_
#include "includes.h"
#include "list_utils.h"
#include "timing_functions.h"
#include "GPIO_handle.h"
//#include "NVS_access.h"
#endif

#define ESPNOW_MAXDELAY 512

// log TAGs
static const char *TAG0 = "main   ";
static const char *TAG1 = "ND task";
static const char *TAG2 = "timer  ";

// queue handles
static QueueHandle_t s_espnow_event_queue;        // espnow event queue
static QueueHandle_t s_time_send_placed_queue;      // queue for timestamps when espnow_send was called

//timer handles
esp_timer_handle_t cycle_timer_handle;

// event group handle and bit
/* EventGroupHandle_t nb_detect_status;
EventBits_t uxBits; */

static uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint16_t s_espnow_seq[ESPNOW_DATA_MAX] = { 0, 0 };

static uint8_t cycle = MASTER_SELECTION;      // flag for which cycle the device is currently in, only used by cycle_timer_cb()

static void espnow_deinit(espnow_send_param_t *send_param);

static void IRAM_ATTR espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    taskENTER_CRITICAL(&spinlock);
    int64_t time_since_boot = esp_timer_get_time();
    taskEXIT_CRITICAL(&spinlock);

    espnow_event_t evt;
    espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
    evt.timestamp = time_since_boot;

    // compute send offset here
    int64_t time_placed;
    if (xQueueReceive(s_time_send_placed_queue, &time_placed, 0) != pdTRUE){
        ESP_LOGW(TAG1, "Receive time_send_placed_queue queue fail");
        send_cb->send_offset = 0;
    }else{
        send_cb->send_offset = time_since_boot - time_placed;
    }

    if (mac_addr == NULL) {
        ESP_LOGE(TAG1, "Send cb arg error");
        return;
    }

    evt.id = ESPNOW_SEND_CB;
    memcpy(send_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    send_cb->status = status;
    if (xQueueSend(s_espnow_event_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
        ESP_LOGW(TAG1, "Send send queue fail");
    }
}

static void IRAM_ATTR espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    taskENTER_CRITICAL(&spinlock);
    int64_t time_now = get_systime_us();
    int64_t recv_offset = esp_timer_get_time() - recv_info->rx_ctrl->timestamp;
    taskEXIT_CRITICAL(&spinlock);

    espnow_event_t evt;
    espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
    recv_cb->sig_len = recv_info->rx_ctrl->sig_len;

    evt.timestamp = time_now - recv_offset;
    
    uint8_t * mac_addr = recv_info->src_addr;

    if (mac_addr == NULL || data == NULL || len <= 0) {
        ESP_LOGE(TAG1, "Receive cb arg error");
        return;
    }

    evt.id = ESPNOW_RECV_CB;
    memcpy(recv_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    recv_cb->data = (uint8_t*) malloc(len);
    if (recv_cb->data == NULL) {
        ESP_LOGE(TAG1, "Malloc receive data fail");
        return;
    }
    memcpy(recv_cb->data, data, len);
    recv_cb->data_len = len;
    if (xQueueSend(s_espnow_event_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
        ESP_LOGW(TAG1, "Send receive queue fail");
        free(recv_cb->data);
    }
}

static void IRAM_ATTR cycle_timer_cb(void* arg){
    portENTER_CRITICAL_ISR(&spinlock);
    switch(cycle){
/*         case (NEIGHBOR_DETECTION):
        {
            // set event bit to exit espnow event loop


        } */
        case (MASTER_SELECTION):
        {
            ESP_ERROR_CHECK( gpio_set_level(GPIO_DEBUG_PIN, 1) );                                           // assert debug pin
            ESP_ERROR_CHECK( esp_timer_start_once(cycle_timer_handle, CONFIG_MSG_EXCHANGE_DURATION_MS*1000) );    // start shutdown timer
            break;
        }
        case (MESSAGE_EXCHANGE):
        {
            ESP_ERROR_CHECK( esp_timer_start_once(cycle_timer_handle, 1000));  // start timer for shutdown cycle
            break;
        }
        case (SHUTDOWN):
        {
            ESP_ERROR_CHECK( esp_timer_delete(cycle_timer_handle));            // delete timer
            ESP_ERROR_CHECK( gpio_set_level(GPIO_DEBUG_PIN, 0));       // deassert debug pin
            
            esp_deep_sleep(CONFIG_DEEPSLEEP_DURATION_MS*1000);          // go to sleep
            break;
        }
        default:
        {
            ESP_LOGE(TAG2, "Timer callback error");
            break;
        }
    }
    cycle++;
    portEXIT_CRITICAL_ISR(&spinlock);
}

void IRAM_ATTR espnow_broadcast_timestamp(espnow_send_param_t *send_param){

    espnow_data_t *buf = (espnow_data_t *)send_param->buffer;
    assert(send_param->len >= sizeof(espnow_data_t));

    buf->type = ESPNOW_DATA_BROADCAST;
    buf->seq_num = s_espnow_seq[buf->type]++;
    
    taskENTER_CRITICAL(&spinlock);
    buf->timestamp = get_systime_us();
    int64_t time_now = esp_timer_get_time();
    if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK) {        // send timestamp
        ESP_LOGE(TAG1, "Send error");
        espnow_deinit(send_param);
        vTaskDelete(NULL);
    }
    taskEXIT_CRITICAL(&spinlock);
    
    if (xQueueSend(s_time_send_placed_queue, &time_now, 0) != pdTRUE){      // give timestamp to queue fior send offset calculation
        ESP_LOGE(TAG1, "Could not post to s_time_send_placed_queue");
        espnow_deinit(send_param);
        vTaskDelete(NULL);
    }

    //ESP_LOGD(TAG1, "broadcasting timestamp");
}

void IRAM_ATTR init_msg_exchange(const int64_t max_offset){
    // load last offset from RTC and display
    int64_t offset = max_offset; //(4*nvs_loadOffset() + max_offset)/5;
    portENTER_CRITICAL(&spinlock);

    // compute delay to start msg exchange in sync with the master time (oldest node);
    int64_t time_now = get_systime_us();
    uint64_t master_time_us = (offset > 0) ? (time_now + offset) : time_now;
    
    uint64_t delay_us = 15000 - (master_time_us % 15000);         // find the delay to the next full 100th of a second (of master if not master)
    
    // start scheduled phases according to master time
    ESP_ERROR_CHECK(esp_timer_start_once(cycle_timer_handle, delay_us)); // set timer for when to start masg exchange
    portEXIT_CRITICAL(&spinlock);
    
    // store new offset to nvs
    //nvs_storeOffset(offset);
}

static void neighbor_detection_task(void *pvParameter){

    int64_t send_offset_sum = 0;
    PeerListHandle_t* peer_list = xInitPeerList();
    TickType_t wait_duration = portMAX_DELAY;
    espnow_send_param_t *send_param = (espnow_send_param_t *)pvParameter;
    
    ESP_LOGI(TAG1, "Starting broadcast discovery of peers");

    // broadcast first timestamp
    espnow_broadcast_timestamp(send_param);

    // start event handle loop 
    espnow_event_t evt;
    while (xQueueReceive(s_espnow_event_queue, &evt, wait_duration) == pdTRUE) {
        switch (evt.id) {
            case ESPNOW_SEND_CB:
            {
                espnow_event_send_cb_t *send_cb = &evt.info.send_cb;

                send_offset_sum += send_cb->send_offset;

                send_param->count--;
                if (send_param->count > 0 && wait_duration != 0) {    	    // send next broadcast if still got some to send
                    
                    if (send_param->delay > 0) {                            // delay before sending next broadcast
                        vTaskDelay(send_param->delay/portTICK_PERIOD_MS);
                    }
                    espnow_broadcast_timestamp(send_param); // broadcast another timestamp

                }else {
                    ESP_LOGW(TAG1, "last broadcast sent");
                    wait_duration = 0;
                }

                break;
            }
            case ESPNOW_RECV_CB:
            {
                espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
                espnow_data_t *recv_data = (espnow_data_t *)(recv_cb->data);

                // compute transmission time and offset 
                int64_t time_TX = recv_data->timestamp;
                int64_t time_RX = evt.timestamp;
                const int64_t time_proc = ( recv_cb->sig_len * 8 ); // processing time of the PHY interface | rate * 10^6 = 1us [in us] 
                int64_t delta = time_TX - time_RX - 2*time_proc;
                
                if (recv_data->type == ESPNOW_DATA_BROADCAST) {
                    //ESP_LOGI(TAG1, "Receive %dth broadcast data from: "MACSTR", len: %d", recv_seq, MAC2STR(recv_cb->mac_addr), recv_cb->data_len);

                    if (esp_now_is_peer_exist(recv_cb->mac_addr) == false) {        // unknown peer
                        
                        vAddPeer(peer_list, recv_cb->mac_addr, delta);

                    }else{                                                          // known peer
                        
                        vAddOffset(peer_list, recv_cb->mac_addr, delta);
                        
                    }
                    
                    if (recv_data->seq_num == CONFIG_ESPNOW_SEND_COUNT-1){
                        wait_duration = 0;
                    }
                
                }
                else if (recv_data->type == ESPNOW_DATA_UNICAST) {
                    ESP_LOGE(TAG1, "Received unicast data : "MACSTR"", MAC2STR(recv_cb->mac_addr));
                }
                else {
                    ESP_LOGE(TAG1, "Receive error data from: "MACSTR"", MAC2STR(recv_cb->mac_addr));
                }
                break;
                
                free(recv_cb->data);
            }
        }
    }

    ESP_LOGW(TAG0, "finished broadcasting at %lld us", esp_timer_get_time());

    // display peer counts and total number of received broadcasts
    ESP_LOGW(TAG1, "Timestamps received from %d peers.", peer_list->peer_count);

    // compute the experienced average send offset
    int64_t avg_send_offset = send_offset_sum / ( CONFIG_ESPNOW_SEND_COUNT - send_param->count ) ;
    ESP_LOGW(TAG1, "avg_send_offset = %lld us", avg_send_offset);

    // determine max offset and coresponding address
    int64_t max_offset = 0;
    uint8_t max_offset_addr[ESP_NOW_ETH_ALEN];
    
    for (uint8_t i = 0; i < ESP_NOW_ETH_ALEN; i++){                 // copy mac address with highest offset from list
        max_offset_addr[i] = peer_list->max_offset_addr[i];
    }

    if (peer_list->peer_count != 0){                                // check if any peers have been detected
        max_offset = peer_list->max_offset - avg_send_offset;       // extract max offset, take average send offset intop account
    }else{
        ESP_LOGE(TAG1, "No peers detected");
    }
    
    vDeletePeerList(peer_list);                                 // delete peer list 
    
    if (max_offset > 0) {                                       // device does not hold the master time 
        ESP_LOGW(TAG1, "Master offset of %lld us by "MACSTR"", max_offset, MAC2STR(max_offset_addr));
        //ESP_LOGW(TAG1, "");
        //ESP_LOGW(TAG1, "deviation to last offset = %lld us", last_offset-max_offset);
        //ESP_LOGW(TAG1, "");
    }else{                                                      // this device holds the master time
        esp_err_t err = esp_read_mac(max_offset_addr, ESP_MAC_WIFI_SOFTAP);
        if(err != ESP_OK){
            ESP_LOGE(TAG1, "Error reading my mac address.");    
        }else{
            ESP_LOGW(TAG1, "I have the master time with "MACSTR"", MAC2STR(max_offset_addr));
        }
    }

    init_msg_exchange(max_offset);
    
    vTaskDelete(NULL);
}

static esp_err_t espnow_init(void)
{
    // wifi init
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(ESPNOW_WIFI_MODE) );
    ESP_ERROR_CHECK( esp_wifi_start());
    ESP_ERROR_CHECK( esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

    // esp now init
    espnow_send_param_t *send_param;

    s_time_send_placed_queue = xQueueCreate(1, sizeof(int64_t));
    s_espnow_event_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(espnow_event_t));
    
    if (s_espnow_event_queue == NULL) {
        ESP_LOGE(TAG1, "creating event queue fail");
        return ESP_FAIL;
    }

    /* Initialize ESPNOW and register sending and receiving callback function. */
    ESP_ERROR_CHECK( esp_now_init() );
    ESP_ERROR_CHECK( esp_now_register_send_cb(espnow_send_cb) );
    ESP_ERROR_CHECK( esp_now_register_recv_cb(espnow_recv_cb) );
    /* Set primary master key. */
    ESP_ERROR_CHECK( esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK) );

    /* Add broadcast peer information to peer list. */
    esp_now_peer_info_t *peer = (esp_now_peer_info_t*) malloc(sizeof(esp_now_peer_info_t));
    if (peer == NULL) {
        ESP_LOGE(TAG1, "Malloc peer information fail");
        vSemaphoreDelete(s_espnow_event_queue);
        esp_now_deinit();
        return ESP_FAIL;
    }
    memset(peer, 0, sizeof(esp_now_peer_info_t));
    peer->channel = CONFIG_ESPNOW_CHANNEL;
    peer->ifidx = ESPNOW_WIFI_IF;
    peer->encrypt = false;
    memcpy(peer->peer_addr, s_broadcast_mac, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK( esp_now_add_peer(peer) );
    free(peer);

    /* Initialize sending parameters. */
    send_param = malloc(sizeof(espnow_send_param_t));
    if (send_param == NULL) {
        ESP_LOGE(TAG1, "Malloc send parameter fail");
        vSemaphoreDelete(s_espnow_event_queue);
        esp_now_deinit();
        return ESP_FAIL;
    }
    memset(send_param, 0, sizeof(espnow_send_param_t));
    send_param->unicast = false;
    send_param->broadcast = true;
    send_param->count = CONFIG_ESPNOW_SEND_COUNT;
    send_param->delay = CONFIG_ESPNOW_SEND_DELAY;
    send_param->len = CONFIG_ESPNOW_SEND_LEN;
    send_param->buffer = (uint8_t*)malloc(CONFIG_ESPNOW_SEND_LEN);
    if (send_param->buffer == NULL) {
        ESP_LOGE(TAG1, "Malloc send buffer fail");
        free(send_param);
        vSemaphoreDelete(s_espnow_event_queue);
        esp_now_deinit();
        return ESP_FAIL;
    }
    memcpy(send_param->dest_mac, s_broadcast_mac, ESP_NOW_ETH_ALEN);

    xTaskCreate(neighbor_detection_task, "neighbor_detection_task", 2048, send_param, 3, NULL);

    return ESP_OK;
}

static void setup_timer(){
 
    // oneshot timer to start cycles on time
    const esp_timer_create_args_t cycle_timer_args = {
            .callback = &cycle_timer_cb,
            .dispatch_method = ESP_TIMER_ISR,
            .name = "cycle_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&cycle_timer_args, &cycle_timer_handle));

}

static void espnow_deinit(espnow_send_param_t *send_param)
{   
    // esp now deinit
    free(send_param->buffer);
    free(send_param);
    vSemaphoreDelete(s_espnow_event_queue);
    esp_now_deinit();

    // wifi deinit
    esp_wifi_stop();
    esp_wifi_deinit();
}

void app_main(void)
{
    
/*     // reset systime if reset did not get triggered by deepsleep
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP){
        ESP_LOGE(TAG1, "Resetting systime - not booting from deepsleep");
        reset_systime();
    } */

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG0, "Resetting flash!");
        ESP_ERROR_CHECK( nvs_flash_erase() );
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );
/* 
    nb_detect_status = xEventGroupCreate();     // create event group 
    assert(nb_detect_status != NULL );
 */
    setup_GPIO();
    setup_timer();
    espnow_init();
}
