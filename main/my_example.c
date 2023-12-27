#ifndef _INCLUDES_H_
#define _INCLUDES_H_
#include "includes.h"
#include "./components/bloomfilter/bloom.c"
#include "list_utils.h"
#include "timing_functions.h"
#include "GPIO_handle.h"
//#include "NVS_access.h"
#endif

#define ESPNOW_MAXDELAY 512
#define CONFIG_ESPNOW_SEND_LEN 11

// log TAGs
static const char *TAG0 = "main   ";
static const char *TAG1 = "nd_task";
static const char *TAG2 = "timer  ";
static const char *TAG3 = "msg_ex ";

// queue handles
static QueueHandle_t espnow_event_queue;          // espnow event queue
static QueueHandle_t s_time_send_placed_queue;      // queue for timestamps when espnow_send was called

//timer handles
esp_timer_handle_t cycle_timer_handle;

static uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };  // broadcast mac address

static uint16_t s_espnow_seq[DATA_UNDEF] = { 0, 0, 0 };       // array with counter of sent packets 
// first  : counter for transmitted timing broadcasts
// second : counter for transmitted reception reports
// third  : counter for transmitted onc packets  

// shared variable with correspnding mutex
SemaphoreHandle_t cycle_mutex;                  // mutex for cycle variable
static uint8_t cycle = NEIGHBOR_DETECTION;      // flag which cycle the device is currently in, shared variable, start with neighbor detection

static void espnow_deinit();

static uint8_t IRAM_ATTR get_cycle(){                   // function for getting share variable cycle
    uint8_t ret;
    xSemaphoreTake(cycle_mutex, portMAX_DELAY);
    ret = cycle;
    xSemaphoreGive(cycle_mutex);
    return ret;
}

static uint8_t IRAM_ATTR get_cycle_ISR(){               // function for getting share variable cycle from ISR
    uint8_t ret;
    xSemaphoreTakeFromISR(cycle_mutex, NULL);
    ret = cycle;
    xSemaphoreGiveFromISR(cycle_mutex, NULL);
    return ret;
}

static void IRAM_ATTR espnow_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    taskENTER_CRITICAL(&spinlock);
    int64_t time_since_boot = esp_timer_get_time();
    int64_t time_now = get_systime_us();
    taskEXIT_CRITICAL(&spinlock);

    if (mac_addr == NULL) {
        ESP_LOGE(TAG1, "Send cb arg error");
        return;
    }

    uint8_t current_cycle = get_cycle();                                    // get cycle currently in
    switch (current_cycle){

        case NEIGHBOR_DETECTION:                                            // in neighbor detection phase
        {                                                         
            espnow_event_t evt;                                             // create event
            espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
            evt.timestamp = time_now;                                       // set timestamp
            evt.id = SEND_TIMESTAMP;                                        // set event id
            
            int64_t time_placed;                                                            
            if (xQueueReceive(s_time_send_placed_queue, &time_placed, 0) != pdTRUE){        // take time when packet was sent from queue
                ESP_LOGE(TAG1, "Receive time_send_placed_queue queue fail");
                send_cb->send_offset = 0;
            }else{
                send_cb->send_offset = time_since_boot - time_placed;                       // compute send offset
            }

            memcpy(send_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);                          // copy mac address
            send_cb->status = status;                                                       // copy status
            
            if (xQueueSend(espnow_event_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {       // place event into queue
                ESP_LOGE(TAG1, "Send send queue fail");
            }
            break;
        }
        default: break;
    }
}

static void IRAM_ATTR espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, uint8_t len)
{
    taskENTER_CRITICAL(&spinlock);
    int64_t time_since_boot = esp_timer_get_time();
    int64_t time_now = get_systime_us();
    taskEXIT_CRITICAL(&spinlock);

    int64_t recv_offset = (recv_info->rx_ctrl->timestamp == 0) ? 0 : time_since_boot - (int64_t)(recv_info->rx_ctrl->timestamp);        // compute receive offset - time between actually receiving packet and time of callback

    espnow_event_t evt;                                 // create event
    evt.timestamp = time_now - recv_offset;             // with timestamp

    uint8_t * mac_addr = recv_info->src_addr;           // get source mac address
    if (mac_addr == NULL || data == NULL || len <= 0) {
        ESP_LOGE(TAG1, "Receive cb arg error");
        return;
    }
    
    espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;    // get recv cb info 
    recv_cb->sig_len = recv_info->rx_ctrl->sig_len;         // set signal length
    memcpy(recv_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);  // set mac address
    
    recv_cb->data = (uint8_t*) malloc(len);         // allocate memory for buffering data
    if (recv_cb->data == NULL) {
        ESP_LOGE(TAG1, "Malloc receive data fail");
        return;
    }

    uint8_t data_type = data[0];                                // select event id
    switch(data_type) {
        case TIMESTAMP:     evt.id = RECV_TIMESTAMP;    break;
        case RECEP_REPORT:  evt.id = RECV_REPORT;       break;
        case ONC_DATA:      evt.id = RECV_ONC_DATA;     break;
        default:            evt.id = EVENT_UNDEF;       break;
    }

    memcpy(recv_cb->data, data, len);               // copy data to buffer
    recv_cb->data_len = len;

    if (xQueueSend(espnow_event_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {    // place event into queue
        ESP_LOGW(TAG1, "Send receive queue fail");
        free(recv_cb->data);
    }
}

static void IRAM_ATTR cycle_timer_cb(void* arg){

    uint8_t current_cycle = get_cycle_ISR();                                                    // get cycle currently in

    switch(current_cycle){

        case NEIGHBOR_DETECTION:
        {
            ESP_ERROR_CHECK( gpio_set_level(GPIO_DEBUG_PIN, 1) );                                                   // assert debug pin
            ESP_ERROR_CHECK( esp_timer_start_once(cycle_timer_handle, CONFIG_MSG_EXCHANGE_DURATION_MS*1000) );      // start shutdown timer
            break;
        }

        case MESSAGE_EXCHANGE:
        {
            ESP_ERROR_CHECK( esp_timer_start_once(cycle_timer_handle, 1000));  // start timer for shutdown cycle
            break;
        }

        case SHUTDOWN:
        {
            ESP_ERROR_CHECK( esp_timer_delete(cycle_timer_handle));     // delete timer
            ESP_ERROR_CHECK( gpio_set_level(GPIO_DEBUG_PIN, 0));        // deassert debug pin
            esp_deep_sleep(CONFIG_DEEPSLEEP_DURATION_MS*1000);          // go to sleep
            break;
        }

        default:
        {
            ESP_LOGE(TAG2, "Timer callback error");
            break;
        }
    }
    cycle++;                                                            // enter next cycle
}

void IRAM_ATTR broadcast_timestamp(){

    timing_data_t buf;

    buf.type = TIMESTAMP;
    buf.seq_num = s_espnow_seq[TIMESTAMP]++;
    
    taskENTER_CRITICAL(&spinlock);
    buf.timestamp = get_systime_us();
    int64_t time_now = esp_timer_get_time();
    esp_err_t err = esp_now_send(s_broadcast_mac, (uint8_t*)&buf, sizeof(timing_data_t));        // send timestamp
    assert(err ==  ESP_OK);
    taskEXIT_CRITICAL(&spinlock);
    
    if (xQueueSend(s_time_send_placed_queue, &time_now, 0) != pdTRUE){      // give timestamp to queue fior send offset calculation
        ESP_LOGE(TAG1, "Could not post to s_time_send_placed_queue");
        espnow_deinit();
        vTaskDelete(NULL);
    }
}

void IRAM_ATTR pseudo_broadcast_report(bloom_t* bloom){
    // create buffer
    uint64_t buf_size = sizeof(reception_report_t) + sizeof(bloom) + bloom->bytes; 
    uint8_t* buf = (uint8_t*)calloc(buf_size, 1);
    assert(buf != NULL);

    // create reception report frame
    reception_report_t report;
    memset(&report, 0, sizeof(reception_report_t));
    report.type = RECEP_REPORT;
    report.seq_num = s_espnow_seq[TIMESTAMP]++;
    
    // copy frame to buffer
    memcpy(buf, &report, sizeof(reception_report_t));

    // serialize bloom filter and also copy to buffer, also free block again
    uint8_t* block = bloom_serialize(bloom);
    assert(block != NULL);
    memcpy(buf+sizeof(reception_report_t), block, sizeof(bloom_t) + bloom->bytes);
    free(block);

    // send data in buf
    esp_err_t err = esp_now_send(NULL, (uint8_t*)&buf, sizeof(timing_data_t));        // send reception report to all peers
    assert(err ==  ESP_OK);

    free(buf);
}

/* void IRAM_ATTR espnow_pseudo_broadcast(const uint8_t *mac_addr_list, encoded_data_t* enc_data){

    // TO DO

    if (esp_now_send(NULL, send_param->buffer, send_param->len) != ESP_OK) {        // pseudo broadcast encoded data
        ESP_LOGE(TAG1, "Send error");
        espnow_deinit(send_param);
        vTaskDelete(NULL);
    }

} */
/* 
static void msg_exchange_task(){    
    ESP_LOGW(TAG3, "Testing bloom filter");

    int64_t max_heap_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    ESP_LOGW(TAG3, "max heap block = %lld", max_heap_block);

    bloom_t ExamplePool;
    int8_t ret = bloom_init2(&ExamplePool, BLOOM_MAX_ENTRIES, BLOOM_ERROR_RATE);
    assert(ret != 1);

    bloom_print(&ExamplePool);

    for (uint16_t i = 0; i < BLOOM_MAX_ENTRIES/2; i++){
        uint32_t number = i*2;
        ret = bloom_add(&ExamplePool, &number, (uint32_t)sizeof(uint32_t));
        assert(ret != -1);
    }

    ESP_LOGE(TAG3, "Size of bloom =       %llu", (uint64_t)sizeof(bloom_t));
    
    uint64_t total_size = sizeof(reception_report_t) + sizeof(bloom_t) + ExamplePool.bytes;
    ESP_LOGE(TAG3, "Size of total block = %llu", total_size);

    vTaskDelay(200/portTICK_PERIOD_MS);    // wait 200 ms

    pseudo_broadcast_report(&ExamplePool);

    // start event handle loop
    espnow_event_t evt;
    while (xQueueReceive(espnow_event_queue, &evt, portMAX_DELAY) == pdTRUE) {
        switch (evt.id) {
            case SEND_REPORT:                                               // WILL NEVER BE CALLED ########### FIX THIS ###########
            {
                espnow_event_send_cb_t *send_cb = &evt.info.send_cb;

                ESP_LOGW(TAG3, "Sent reception pool to"MACSTR"", MAC2STR(send_cb->mac_addr));
                break;
            }
            case RECV_REPORT:
            {
                espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
                reception_report_t *recv_data = (reception_report_t*)(recv_cb->data);

                if (recv_data->type != RECEP_REPORT){
                    ESP_LOGE(TAG3, "Call back error");
                    break;
                }

                // unwrap bloom filter from receive data
                bloom_t* bloom = (bloom_t*)recv_data->bloom;
                bloom->bf = recv_data->bloom+sizeof(bloom_t); 

                // do some tests
                bloom_print(bloom);

                for (uint16_t i = 0; i < BLOOM_MAX_ENTRIES/2; i++){
                    uint32_t number = i*2;
                    int8_t ret = bloom_check(&ExamplePool, &number, (uint32_t)sizeof(uint32_t));
                    if (ret == 0) ESP_LOGW(TAG3, "NOT FOUND");
                    else ESP_LOGW(TAG3, "FOUND IT");
                }

                //vReplaceReport(peer_list, bloom, recv_cb->mac_addr);
                free(recv_data);

                pseudo_broadcast_report(&ExamplePool);
                break;
            }
            case RECV_TIMESTAMP:
            case RECV_ONC_DATA:                                             // delete data not related to receiption reports 
            {                
                espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;   
                uint8_t recv_type = recv_cb->data[0];
                if ( (recv_type != RECV_ONC_DATA) ||  (recv_type != RECV_TIMESTAMP) ){
                    ESP_LOGE(TAG3, "Call back error");
                    break;
                }

                free(recv_cb->data);
                break;
            }
            case SEND_TIMESTAMP:
            case SEND_ONC_DATA:
            default: break;
        }   
    }
}
 */
void IRAM_ATTR init_msg_exchange(const int64_t max_offset){

    taskENTER_CRITICAL(&spinlock);
    int64_t offset = max_offset; //(4*nvs_loadOffset() + max_offset)/5;

    // compute delay to start msg exchange in sync with the master time (oldest node);
    int64_t time_now = get_systime_us();
    uint64_t master_time_us = (offset > 0) ? (time_now + offset) : time_now;
    
    uint64_t delay_us = 10000 - (master_time_us % 10000);         // find the delay to the next time intervall start (of master if not master)
    
    // start scheduled phases according to master time
    ESP_ERROR_CHECK(esp_timer_start_once(cycle_timer_handle, delay_us)); // set timer for when to start masg exchange
    taskEXIT_CRITICAL(&spinlock);
    
    // create msg exchange task
    //xTaskCreate(msg_exchange_task, "msg_exchange_task", 4096, NULL, 3, NULL);
    
    // store new offset to nvs
    //nvs_storeOffset(offset);
}

static void neighbor_detection_task(){

    int64_t send_offset_sum = 0;
    uint16_t broadcasts_sent = 0;
    PeerListHandle_t* peer_list = xInitPeerList();
    TickType_t wait_duration = portMAX_DELAY;

    ESP_LOGI(TAG1, "Starting broadcast discovery of peers");

    // broadcast first timestamp
    broadcast_timestamp();

    // start event handle loop
    espnow_event_t evt;
    while (xQueueReceive(espnow_event_queue, &evt, wait_duration) == pdTRUE) {
        switch (evt.id) {
            case SEND_TIMESTAMP:
            {
                espnow_event_send_cb_t *send_cb = &evt.info.send_cb;

                send_offset_sum += send_cb->send_offset;

                broadcasts_sent++;
                if (broadcasts_sent < CONFIG_TIMESTAMP_SEND_COUNT && wait_duration != 0) {    	    // send next broadcast if still got some to send and there is still time
                    
                    if (CONFIG_TIMESTAMP_SEND_DELAY > 0) {                                          // delay before sending next broadcast
                        vTaskDelay(CONFIG_TIMESTAMP_SEND_DELAY/portTICK_PERIOD_MS);
                    }
                    broadcast_timestamp(); // broadcast another timestamp

                }else {
                    ESP_LOGW(TAG1, "last broadcast sent");
                    wait_duration = 0;
                }

                break;
            }
            case RECV_TIMESTAMP:
            {
                espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
                timing_data_t *recv_data = (timing_data_t *)(recv_cb->data);

                if (recv_data->type != TIMESTAMP) {
                    ESP_LOGE(TAG1, "Received timestamp event, but data is not a timestamp");
                    break;
                }

                // compute transmission time and offset 
                int64_t time_TX = recv_data->timestamp;
                int64_t time_RX = evt.timestamp;
                const int64_t time_proc = ( recv_cb->sig_len * 8 ); // processing time of the PHY interface | rate * 10^6 = 1us [in us] 
                int64_t delta = time_TX - time_RX - 2*time_proc;
                
                if (esp_now_is_peer_exist(recv_cb->mac_addr) == false) {        // unknown peer
                    
                    vAddPeer(peer_list, recv_cb->mac_addr, delta);

                }else{                                                          // known peer
                    
                    vAddOffset(peer_list, recv_cb->mac_addr, delta);
                    
                }
                
                if (recv_data->seq_num == CONFIG_TIMESTAMP_SEND_COUNT-1){
                    wait_duration = 0;
                }
                break;
            }
            case RECV_ONC_DATA:                                 
            case RECV_REPORT:                                                   // delete data not related to timestamps
            {
                espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;        
                uint8_t recv_type = recv_cb->data[0];
                if (( recv_type != RECV_ONC_DATA) || (recv_type != RECV_REPORT) ){
                    ESP_LOGE(TAG3, "Call back error");
                    break;
                }
                
                free(recv_cb->data);
                break;
            }
            case SEND_ONC_DATA:
            case SEND_REPORT:
            default: break;
        }
    }

    ESP_LOGW(TAG0, "finished broadcasting at %lld us", esp_timer_get_time());

    // display peer counts and total number of received broadcasts
    ESP_LOGW(TAG1, "Timestamps received from %d peers.", peer_list->peer_count);

    // compute the experienced average send offset
    int64_t avg_send_offset = send_offset_sum / broadcasts_sent;
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
    ESP_ERROR_CHECK( esp_wifi_set_ps(WIFI_PS_NONE) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(ESPNOW_WIFI_MODE) );
    ESP_ERROR_CHECK( esp_wifi_start());
    ESP_ERROR_CHECK( esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

    // esp now init
    s_time_send_placed_queue = xQueueCreate(1, sizeof(int64_t));
    espnow_event_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(espnow_event_t));
    if (espnow_event_queue == NULL) {
        ESP_LOGE(TAG1, "creating event queue fail");
        return ESP_FAIL;
    }

    /* initialize ESPNOW and register sending and receiving callback function. */
    ESP_ERROR_CHECK( esp_now_init() );
    ESP_ERROR_CHECK( esp_now_register_send_cb(espnow_send_cb) );
    ESP_ERROR_CHECK( esp_now_register_recv_cb(espnow_recv_cb) );
    /* Set primary master key. */
    ESP_ERROR_CHECK( esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK) );

    /* add broadcast peer information to peer list. */
    esp_now_peer_info_t *peer = (esp_now_peer_info_t*) malloc(sizeof(esp_now_peer_info_t));
    if (peer == NULL) {
        ESP_LOGE(TAG1, "Malloc peer information fail");
        vSemaphoreDelete(espnow_event_queue);
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

    xTaskCreate(neighbor_detection_task, "neighbor_detection_task", 2048, NULL, 3, NULL);

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

static void espnow_deinit()
{   
    vSemaphoreDelete(espnow_event_queue);
    esp_now_deinit();

    // wifi deinit
    esp_wifi_stop();
    esp_wifi_deinit();
}

void app_main(void)
{
    ESP_LOGW(TAG0, "boot time = %lld", esp_timer_get_time());
    
    /*// reset systime if reset did not get triggered by deepsleep
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP){
        ESP_LOGE(TAG1, "Resetting systime - not booting from deepsleep");
        reset_systime();
    } */

    esp_err_t ret = nvs_flash_init();                                                   // init flash
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {     // erase if error occured
        ESP_LOGE(TAG0, "Resetting flash!");
        ESP_ERROR_CHECK( nvs_flash_erase() );
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    cycle_mutex = xSemaphoreCreateMutex();      // create mutex

    setup_GPIO();
    setup_timer();
    espnow_init();
}
