#ifndef _INCLUDES_H_
#define _INCLUDES_H_
#include "includes.h"
#include "./components/bloomfilter/bloom.c"
//#include "./components/linreg/linreg.h"
#include "peer_management.h"
#include "timing_functions.h"
//#include "NVS_access.h"
#endif

#define ESPNOW_MAXDELAY 512
#define CONFIG_ESPNOW_SEND_LEN 11

#define GPIO_DEBUG_PIN          18
#define GPIO_START_PIN          0
#define GPIO_DEBUG_PIN_SEL      (1ULL<<GPIO_DEBUG_PIN)
#define GPIO_START_PIN_SEL      (1ULL<<GPIO_START_PIN)

// log TAGs
static const char *TAG0 = "main   ";
static const char *TAG1 = "nd_task";
static const char *TAG2 = "timer  ";
static const char *TAG3 = "init_msg_ex";
static const char *TAG4 = "msg_ex ";

// task handles
static TaskHandle_t network_reset_task_handle      = NULL;
static TaskHandle_t network_sync_task_handle       = NULL;
static TaskHandle_t neighbor_detection_task_handle = NULL;

// queue handles
static QueueHandle_t espnow_event_queue;        // espnow event queue
static QueueHandle_t avg_send_offset_queue;     // queue for current average send offset
static QueueHandle_t avg_recv_offset_queue;     // queue for current average recv offset
static QueueHandle_t last_commission_queue;     // queue for keeping track of last commission
static QueueHandle_t output_queue;              //               

//timer handles
static esp_timer_handle_t cycle_timer_handle;

// event group handle
static EventGroupHandle_t CycleEventGroup;

static uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };  // broadcast mac address

// first  : counter for transmitted timing broadcasts
// second : counter for transmitted reception reports
// third  : counter for transmitted onc packets  

// time when wifi counter start - for receive offset correction
int64_t time_wifi_start = 0;

static void espnow_deinit();

static EventBits_t IRAM_ATTR getCycle(){                        // function for getting cycle through event group    
    EventBits_t evtBits = xEventGroupGetBits(CycleEventGroup);
    return evtBits;
}

static EventBits_t IRAM_ATTR getCycleFromISR(){                 // function for getting cycle through event group from interrupt
    EventBits_t evtBits = xEventGroupGetBitsFromISR(CycleEventGroup);
    return evtBits;
}

static void IRAM_ATTR startCycle(){                             // function for starting cycle
    //xEventGroupClearBits(CycleEventGroup, NETWORK_RESET);
    xEventGroupSetBits(CycleEventGroup, NEIGHBOR_DETECTION);
    if (!esp_timer_is_active(cycle_timer_handle)){
        ESP_ERROR_CHECK( esp_timer_start_once(cycle_timer_handle, CONFIG_BROADCAST_DURATION*1000) );
    }
    //ESP_ERROR_CHECK( esp_timer_start_once(cycle_timer_handle, CONFIG_BROADCAST_DURATION*1000) );
}

static void IRAM_ATTR nextCycleFromISR(){                       // function for setting event bits for next cycle
    BaseType_t HigherPriorityTaskWoken, ret;
    EventBits_t currBits = xEventGroupGetBitsFromISR(CycleEventGroup);
    EventBits_t nextBits = (currBits<<1)|(1<<0);
    ret = xEventGroupSetBitsFromISR(CycleEventGroup, nextBits, &HigherPriorityTaskWoken);
    if (ret == pdPASS) portYIELD_FROM_ISR(HigherPriorityTaskWoken);
    assert(ret != pdFALSE);
}

static bool IRAM_ATTR blockUntilCycle(EventBits_t cycle){
    EventBits_t EventBits;
    EventBits = xEventGroupWaitBits(CycleEventGroup, cycle, pdFALSE, pdTRUE, portMAX_DELAY);        // block until init msg exchange event is set
    return (EventBits | cycle) == cycle;
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

    last_commission_t last_commission = {0};
    if (xQueueReceive(last_commission_queue, &last_commission, 0) != pdTRUE){    // get last commission feedback
        ESP_LOGE(TAG1, "Receive last_commission_queue fail");
        return;
    }

    espnow_event_t evt;                                             // create event
    espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
    evt.timestamp = time_now;                                       // set timestamp

    switch(last_commission.packet_type){                            // set event id
        case TIMESTAMP:     evt.id = SEND_TIMESTAMP; break;
        case ONC_DATA:      evt.id = SEND_TIMESTAMP; break;
        case RECEP_REPORT:  evt.id = SEND_ONC_DATA;  break;
        case RESET:         evt.id = SEND_RESET;     break;
        default: break;
    }

    send_cb->send_offset = (last_commission.time_placed == 0) ? 0 : (time_since_boot-last_commission.time_placed);  // compute send offset

    memcpy(send_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);  // copy mac address
    send_cb->status = status;                               // copy status

    if (xQueueSend(espnow_event_queue, &evt, 5/portTICK_PERIOD_MS) != pdTRUE) {       // place event into queue
        ESP_LOGE(TAG1, "Send send queue fail");
    }
}

static void IRAM_ATTR espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, uint8_t len)
{
    taskENTER_CRITICAL(&spinlock);
    int64_t time_since_boot = esp_timer_get_time();
    int64_t time_now = get_systime_us();
    taskEXIT_CRITICAL(&spinlock);

    int64_t recv_offset = (recv_info->rx_ctrl->timestamp == 0) ? 0 : time_since_boot - (int64_t)(recv_info->rx_ctrl->timestamp) - time_wifi_start;        // compute receive offset - timestamp from wifi_pkt_rx_ctrl_t is super unreliable
    
    uint8_t data_type = data[0];                                                             // type of packet
    if (getCycle() == HOLD){ 
        if ((data_type == TIMESTAMP) && ((timing_data_t*)data)->seq_num == 0){               // if on hold and found the first timestamp of a peer
            xEventGroupSetBits(CycleEventGroup, INIT_DETECTION);                             // start sync cycle
        }
        return;
    }

    espnow_event_t evt;                                 // create event
    evt.timestamp = time_now;                           // with timestamp

    uint8_t * mac_addr = recv_info->src_addr;           // get source mac address
    if (mac_addr == NULL || data == NULL || len <= 0) {
        ESP_LOGE(TAG1, "Receive cb arg error");
        return;
    }
    
    espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;    // get recv cb info 
    recv_cb->sig_len = recv_info->rx_ctrl->sig_len;         // set signal length
    memcpy(recv_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);  // set mac address
    recv_cb->recv_offset = recv_offset;                     // save receive offset

    recv_cb->data = (uint8_t*) malloc(len);         // allocate memory for buffering data
    if (recv_cb->data == NULL) {
        ESP_LOGE(TAG1, "Malloc receive data fail");
        return;
    }

    switch(data_type) {
        case TIMESTAMP:     evt.id = RECV_TIMESTAMP;    break;
        case RECEP_REPORT:  evt.id = RECV_REPORT;       break;
        case ONC_DATA:      evt.id = RECV_ONC_DATA;     break;
        case RESET:
        {
            free(recv_cb->data);
            xEventGroupSetBits(CycleEventGroup, NETWORK_RESET);
            esp_restart();
            break;
        }
        default:            
        {
            ESP_LOGE("recv_cb","Received undefinded packet!");
            return; 
        }
    }

    memcpy(recv_cb->data, data, len);               // copy data to buffer
    recv_cb->data_len = len;

    if (xQueueSend(espnow_event_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {    // place event into queue
        ESP_LOGW(TAG1, "Send receive queue fail");
        free(recv_cb->data);
    }
}

static void IRAM_ATTR cycle_timer_isr(void* arg){

    switch(getCycleFromISR()){

        //case INIT_DETECTION: return;

        case NEIGHBOR_DETECTION:
        {
            break;
        }

        case INIT_MSG_EXCHANGE:
        {
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

        case NETWORK_RESET: return;

        default:
        {
            assert(1==0);
            return;
        }
    }
    nextCycleFromISR();
}

static void IRAM_ATTR start_manually_isr(void* arg){
    if (getCycleFromISR() == HOLD){
        BaseType_t HigherPriorityTaskWoken, ret;
        ret = xEventGroupSetBitsFromISR(CycleEventGroup, NEIGHBOR_DETECTION, &HigherPriorityTaskWoken);
        if (ret == pdPASS) portYIELD_FROM_ISR(HigherPriorityTaskWoken);
    }else{
        BaseType_t HigherPriorityTaskWoken, ret;
        ret = xEventGroupSetBitsFromISR(CycleEventGroup, NETWORK_RESET, &HigherPriorityTaskWoken);
        if (ret == pdPASS) portYIELD_FROM_ISR(HigherPriorityTaskWoken);
    }
}

static void IRAM_ATTR broadcast_reset(){
    uint8_t buf = RESET;

    // broadcast reset
    taskENTER_CRITICAL(&spinlock);
    int64_t time_now = esp_timer_get_time();
    ESP_ERROR_CHECK( esp_now_send(s_broadcast_mac, (uint8_t*)&buf, sizeof(timing_data_t)) );
    taskEXIT_CRITICAL(&spinlock);

    // log commission
    last_commission_t new_comm;
    new_comm.packet_type = RESET;
    new_comm.time_placed = time_now;
    if (xQueueSend(last_commission_queue, &new_comm, 5/portTICK_PERIOD_MS) != pdTRUE){       // set next commissioned transmission type to timestamp
        assert(1 == 0);
    }
}

static void IRAM_ATTR broadcast_timestamp(uint16_t seq_num){

    timing_data_t buf;

    buf.type = TIMESTAMP;
    buf.seq_num = seq_num;

    // broadcast new timestamp
    taskENTER_CRITICAL(&spinlock);
    int64_t time_now = esp_timer_get_time();
    buf.timestamp = get_systime_us();
    ESP_ERROR_CHECK( esp_now_send(s_broadcast_mac, (uint8_t*)&buf, sizeof(timing_data_t)) );         
    taskEXIT_CRITICAL(&spinlock);

    // log the new commission
    last_commission_t new_comm;
    new_comm.packet_type = TIMESTAMP;
    new_comm.time_placed = time_now;
    if (xQueueSend(last_commission_queue, &new_comm, 5/portTICK_PERIOD_MS) != pdTRUE){       // set next commissioned transmission type to timestamp
        assert(1 == 0);
    }
}
 
static void IRAM_ATTR send_native_packet(const uint8_t *mac_addr){
    native_data_t buff;

    buff.type = NATIVE_DATA;
    esp_fill_random(buff.seq_num, sizeof(uint16_t));                                                     // random sequence number
    esp_fill_random(&buff.payload, PAYLOAD_LEN);                                                         // fill with random data
    buff.crc16 = esp_crc16_le(UINT16_MAX, (uint8_t const *)buff.payload, PAYLOAD_LEN*sizeof(uint8_t));    // compute crc over payload

    ESP_ERROR_CHECK( esp_now_send(mac_addr, (uint8_t*)&buff, sizeof(native_data_t)));

    // log the new commission
    last_commission_t new_comm;
    new_comm.packet_type = NATIVE_DATA;
    new_comm.time_placed = esp_timer_get_time();
    if (xQueueSend(last_commission_queue, &new_comm, 0) != pdTRUE){       // set next commissioned transmission
        assert(1 == 0);
    }
}

static void IRAM_ATTR send_onc_packet(const uint8_t *mac_addr){
    
    onc_data_t onc_pckt = xGenerateCodedPaket();
    
    // wrap data into a frame
    size_t pckt_size = 2*sizeof(uint8_t) + onc_pckt.pckt_cnt*sizeof(uint16_t) + sizeof(native_data_t);       // packet size in bytes
    uint8_t* buff = (uint8_t*) malloc(pckt_size);
    assert(buff != NULL);

    memcpy(&buff, &onc_pckt.type, 2*sizeof(uint8_t));                                                // copy type and packet count into buffer
    memcpy(&buff[2*sizeof(uint8_t)], onc_pckt.pckt_id_array, onc_pckt.pckt_cnt*sizeof(uint16_t));    // copy packet id array into buffer
    memcpy(&buff[2*sizeof(uint8_t)+onc_pckt.pckt_cnt*sizeof(uint16_t)], onc_pckt.encoded_data, sizeof(native_data_t));

    ESP_ERROR_CHECK( esp_now_send(mac_addr, &buff, pckt_size) );

    free(onc_pckt.pckt_id_array);
    free(buff);

    // log the new commission
    last_commission_t new_comm;
    new_comm.packet_type = ONC_DATA;
    new_comm.time_placed = esp_timer_get_time();
    if (xQueueSend(last_commission_queue, &new_comm, 0) != pdTRUE){       // set next commissioned transmission
        assert(1 == 0);
    }
}

/* 
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

void IRAM_ATTR espnow_pseudo_broadcast(const uint8_t *mac_addr_list, encoded_data_t* enc_data){

    // TO DO

    if (esp_now_send(NULL, send_param->buffer, send_param->len) != ESP_OK) {        // pseudo broadcast encoded data
        ESP_LOGE(TAG1, "Send error");
        espnow_deinit(send_param);
        vTaskDelete(NULL);
    }

} */

static void msg_exchange_task(){
    if (blockUntilCycle(MESSAGE_EXCHANGE) == false) vTaskDelete(NULL);

    ESP_ERROR_CHECK( gpio_set_level(GPIO_DEBUG_PIN, 1) );                                                   // assert debug pin
    
    vTaskDelete(NULL);
    /* ESP_LOGW(TAG4, "Testing bloom filter");

    int64_t max_heap_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    ESP_LOGW(TAG4, "max heap block = %lld", max_heap_block);

    bloom_t ExamplePool;
    int8_t ret = bloom_init2(&ExamplePool, BLOOM_MAX_ENTRIES, BLOOM_ERROR_RATE);
    assert(ret != 1);

    bloom_print(&ExamplePool);

    for (uint16_t i = 0; i < BLOOM_MAX_ENTRIES/2; i++){
        uint32_t number = i*2;
        ret = bloom_add(&ExamplePool, &number, (uint32_t)sizeof(uint32_t));
        assert(ret != -1);
    }

    ESP_LOGE(TAG4, "Size of bloom =       %llu", (uint64_t)sizeof(bloom_t));
    
    uint64_t total_size = sizeof(reception_report_t) + sizeof(bloom_t) + ExamplePool.bytes;
    ESP_LOGE(TAG4, "Size of total block = %llu", total_size);

    vTaskDelay(200/portTICK_PERIOD_MS);    // wait 200 ms

    pseudo_broadcast_report(&ExamplePool);

    // start event handle loop
    espnow_event_t evt;
    while (xQueueReceive(espnow_event_queue, &evt, portMAX_DELAY) == pdTRUE) {
        switch (evt.id) {
            case SEND_REPORT:                                               // WILL NEVER BE CALLED ########### FIX THIS ###########
            {
                espnow_event_send_cb_t *send_cb = &evt.info.send_cb;

                ESP_LOGW(TAG4, "Sent reception pool to"MACSTR"", MAC2STR(send_cb->mac_addr));
                break;
            }
            case RECV_REPORT:
            {
                espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
                reception_report_t *recv_data = (reception_report_t*)(recv_cb->data);

                if (recv_data->type != RECEP_REPORT){
                    ESP_LOGE(TAG4, "Call back error");
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
                    if (ret == 0) ESP_LOGW(TAG4, "NOT FOUND");
                    else ESP_LOGW(TAG4, "FOUND IT");
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
                    ESP_LOGE(TAG4, "Call back error");
                    break;
                }

                free(recv_cb->data);
                break;
            }
            case SEND_TIMESTAMP:
            case SEND_ONC_DATA:
            default: break;
        }   
    } */
}

static void IRAM_ATTR network_sync_task(void* arg){
    if (blockUntilCycle(INIT_MSG_EXCHANGE) == false) vTaskDelete(NULL);     // delete if unblocking because of higher Cycle 

    // create msg exchange task
    xTaskCreate(msg_exchange_task, "msg_exchange_task", 4096, NULL, 3, NULL);

    taskENTER_CRITICAL(&spinlock);
    //int64_t time_called = esp_timer_get_time();
    //ESP_LOGW(TAG3, "Called Init at %lld us", time_called);
    // get current average send offset from neighbor detection task
    int64_t avg_send_offset = 0;
    if (xQueuePeek(avg_send_offset_queue, &avg_send_offset, 0) != pdTRUE){
        //ESP_LOGE(TAG3, "No send offset available!");
    }

    int64_t avg_recv_offset = 0;
    if (xQueuePeek(avg_recv_offset_queue, &avg_recv_offset, 0) != pdTRUE){
        //ESP_LOGE(TAG3, "No recv offset available!");
    }
    
    // determine peer_count, max offset and coresponding peer address
    int64_t max_offset = getMaxOffset();
    uint8_t peer_count = getPeerCount();
    uint8_t max_offset_addr[ESP_NOW_ETH_ALEN];
    getMaxOffsetAddr(max_offset_addr);

    if (peer_count != 0){                                                     // check if any peers have been detected
        max_offset = max_offset - (avg_send_offset/2 -8);                            // extract max offset, take average send offset in to account
    }
    
    // compute delay to start msg exchange in sync with the master time (oldest node);
    int64_t my_time = get_systime_us();
    uint64_t master_time_us = (max_offset > 0) ? (my_time + max_offset) : my_time;
    uint64_t delay_us = 50000L - (master_time_us % 50000L);                             // compute transit time for msg exchange
    
    // start scheduled phases according to master time
    ESP_ERROR_CHECK( esp_timer_start_once(cycle_timer_handle, delay_us) );              // set timer for when to start masg exchange
    taskEXIT_CRITICAL(&spinlock);
    
    // print stats
    ESP_LOGW(TAG3, "Systime at %lld us", my_time);
    ESP_LOGW(TAG3, "avg_send_offset = %lld us", avg_send_offset);
    ESP_LOGW(TAG3, "avg_recv_offset = %lld us", avg_recv_offset);
    ESP_LOGW(TAG3, "Timestamps received from %d peers.", peer_count);
    
    if (max_offset > 0) {                                       // device does not hold the master time 
        ESP_LOGW(TAG3, "Offset to master with %lld us by "MACSTR"", max_offset, MAC2STR(max_offset_addr));
    }else{                                                      // this device holds the master time
        ESP_ERROR_CHECK( esp_read_mac(max_offset_addr, ESP_MAC_WIFI_SOFTAP) );
        ESP_LOGW(TAG3, "I have the master time with "MACSTR"", MAC2STR(max_offset_addr));
    }
    ESP_LOGE(TAG3, "computed delay %lld us", delay_us);

    //vDeletePeerList(peer_list);                                          // delete peer list 
    vTaskDelete(NULL);
}

static void IRAM_ATTR neighbor_detection_task(void* arg){
    int64_t send_offset_sum = 0;
    int64_t recv_offset_sum = 0;
    uint16_t sent_offset_cnt = 0;
    uint16_t recv_offset_cnt = 0;
    uint16_t broadcasts_sent = 0;
    
    ESP_LOGI(TAG1, "Starting broadcast discovery of peers");

    // broadcast first timestamp
    broadcast_timestamp(broadcasts_sent);

    // start event handle loop
    espnow_event_t evt;
    while (xQueueReceive(espnow_event_queue, &evt, portMAX_DELAY) == pdTRUE) {
        if (getCycle() != NEIGHBOR_DETECTION) break; 
        
        switch (evt.id) {
            case SEND_TIMESTAMP:
            {
                espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
                
                broadcasts_sent++;

                if (send_cb->send_offset != 0){                                             // post current average send offset to queue
                    send_offset_sum += send_cb->send_offset;
                    int64_t avg_send_offset = send_offset_sum / ++sent_offset_cnt / 2;
                    xQueueOverwrite(avg_send_offset_queue, &avg_send_offset);
                }

                if (CONFIG_TIMESTAMP_SEND_DELAY > 0) vTaskDelay(CONFIG_TIMESTAMP_SEND_DELAY/portTICK_PERIOD_MS);    // wait for some interval 
                broadcast_timestamp(broadcasts_sent);                                                               // broadcast another timestamp
                
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

                if (recv_cb->recv_offset != 0){
                    recv_offset_sum += recv_cb->recv_offset;
                    int64_t avg_recv_offset = recv_offset_sum / ++recv_offset_cnt;
                    xQueueOverwrite(avg_recv_offset_queue, &avg_recv_offset);         // post current average receive offset to queue
                }else{
                    ESP_LOGE(TAG1, "Receive offset missing!");
                }
                
                // compute transmission time and systime offset to peer 
                int64_t time_TX = recv_data->timestamp;
                int64_t time_RX = evt.timestamp;
                //const int64_t time_tm = (recv_cb->sig_len) * 8 / 54;                      // transmission delay [us] = framesize / bandwidth = ( 54 * 8 ) / ( 54 * 1e6) *1e6 = 8 us 

                int64_t delta = time_TX - time_RX;

                if (boPeerExists(recv_cb->mac_addr) == false) {        // unknown peer
                    vAddPeer(recv_cb->mac_addr, delta);
                }else{                                                          // known peer
                    vAddOffset(recv_cb->mac_addr, delta);
                }
                
                break;
            }
            case RECV_ONC_DATA:                                 
            case RECV_REPORT:                                                   // delete data not related to timestamps
            {
                espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;            
                uint8_t recv_type = recv_cb->data[0];
                if (( recv_type != RECV_ONC_DATA) || (recv_type != RECV_REPORT) ){
                    ESP_LOGE(TAG1, "Call back error");
                    break;
                }
                
                free(recv_cb->data);
                break;
            }
            default: break;
        }
    }
    ESP_LOGW(TAG1, "broadcasts_sent = %u", broadcasts_sent);
    vTaskDelete(NULL);
}

static void IRAM_ATTR network_reset_task(void* arg){
    
    if (blockUntilCycle(NETWORK_RESET) == false) vTaskDelete(NULL);
    
    printf("\n");
    ESP_LOGE("network reset", "RESETTING NETWORK");
    printf("\n");

    for (uint8_t i = 0; i < 10; i++){
        broadcast_reset();
        vTaskDelay(10/portTICK_PERIOD_MS);
    }
    esp_now_deinit();
    esp_restart();
}

static void deinit_neighbor_detection(void){
    vQueueDelete(avg_send_offset_queue);
    vQueueDelete(avg_recv_offset_queue);
    // what else ???????
}

static void espnow_init(void)
{
    // wifi init
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    //ESP_ERROR_CHECK( esp_wifi_set_country(??????));
    ESP_ERROR_CHECK( esp_wifi_set_ps(WIFI_PS_NONE) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_FLASH) );        // LEAVE THIS TO FLASH!!! if RAM is configured here, everything smh becomes super slow 
    ESP_ERROR_CHECK( esp_wifi_set_mode(ESPNOW_WIFI_MODE) );
    ESP_ERROR_CHECK( esp_wifi_start());
    time_wifi_start = esp_timer_get_time();
    ESP_ERROR_CHECK( esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
    
    // init espnow event queue
    espnow_event_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(espnow_event_t));
    assert(espnow_event_queue != NULL);
    last_commission_queue = xQueueCreate(1, sizeof(last_commission_t));
    assert(last_commission_queue != NULL);

    /* initialize ESPNOW and register sending and receiving callback function. */
    ESP_ERROR_CHECK( esp_now_init() );
    ESP_ERROR_CHECK( esp_now_register_send_cb(espnow_send_cb) );
    ESP_ERROR_CHECK( esp_now_register_recv_cb(espnow_recv_cb) );
    /* Set primary master key. */
    ESP_ERROR_CHECK( esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK) );

    /* add broadcast peer information to peer list. */
    esp_now_peer_info_t *peer = (esp_now_peer_info_t*) malloc(sizeof(esp_now_peer_info_t));
    assert(peer != NULL);

    memset(peer, 0, sizeof(esp_now_peer_info_t));
    peer->channel = CONFIG_ESPNOW_CHANNEL;
    peer->ifidx = ESPNOW_WIFI_IF;
    peer->encrypt = false;
    memcpy(peer->peer_addr, s_broadcast_mac, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK( esp_now_add_peer(peer) );
    free(peer);

}

static void setup_timer(void){
    
    // oneshot timer to start cycles on time
    const esp_timer_create_args_t cycle_timer_args = {
            .callback = &cycle_timer_isr,
            .dispatch_method = ESP_TIMER_ISR,
            .name = "cycle_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&cycle_timer_args, &cycle_timer_handle));
}

static void setup_GPIO(void){
    gpio_config_t io_conf = {};                     // zero initialise config  structure

    // gpo config for measuring time differnces at observer 
    io_conf.intr_type = GPIO_INTR_DISABLE;          // disable interrupt
    io_conf.mode = GPIO_MODE_OUTPUT;                // set as output mode
    io_conf.pin_bit_mask = GPIO_DEBUG_PIN_SEL;      // bit mask of the pin
    io_conf.pull_down_en = 1;                       // enable pull-down mode
    io_conf.pull_up_en = 0;                         // disable pull-up mode
    ESP_ERROR_CHECK( gpio_config(&io_conf) );       // configure the GPIO with the given settings

    // gpi interrupt for detection onboard button press reset and sync network
    io_conf.intr_type = GPIO_INTR_NEGEDGE;          // detect rising edge
    io_conf.mode = GPIO_MODE_INPUT;                 // set as input mode
    io_conf.pin_bit_mask = GPIO_START_PIN_SEL;      // bit mask of the pin
    io_conf.pull_down_en = 0;                       // disable pull-up mode (GPIO0 has a hardware pull up)
    io_conf.pull_up_en = 1;                         // disable pull-up mode
    ESP_ERROR_CHECK( gpio_config(&io_conf) );       // configure the GPIO with the given settings
    ESP_ERROR_CHECK( gpio_install_isr_service(0) );                                 // install gpio isr service
    ESP_ERROR_CHECK( gpio_isr_handler_add(GPIO_START_PIN, start_manually_isr, NULL) ); // hook isr handler for GPIO0

    //ESP_ERROR_CHECK( esp_sleep_enable_ext0_wakeup(GPIO_START_PIN, 0) );
}

static void espnow_deinit()
{   
    esp_now_deinit();

    // wifi deinit
    esp_wifi_stop();
    esp_wifi_deinit();

    // delete queues
    vQueueDelete(last_commission_queue);
    vQueueDelete(espnow_event_queue);
}

void app_main(void)
{
    setup_GPIO();

    CycleEventGroup = xEventGroupCreate();      // create event group
    assert(CycleEventGroup != NULL);

    esp_err_t ret = nvs_flash_init();                                                   // init flash
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {     // erase if error occured
        ESP_LOGE(TAG0, "Resetting flash!");
        ESP_ERROR_CHECK( nvs_flash_erase() );
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    setup_timer();
    espnow_init();

    // initialise peer list 
    vInitPeerList();      

    // init needed queues
    avg_send_offset_queue  = xQueueCreate(1, sizeof(int64_t));
    assert(avg_send_offset_queue != NULL);
    avg_recv_offset_queue  = xQueueCreate(1, sizeof(int64_t));
    assert(avg_recv_offset_queue != NULL);

    // if reset did not get triggered by deepsleep set systime so esps will be futher apart
    if (esp_reset_reason() != ESP_RST_DEEPSLEEP){
        /* uint8_t mac_addr[ESP_NOW_ETH_ALEN];                                                 // derive systime from mac address
        ESP_ERROR_CHECK( esp_read_mac(mac_addr, ESP_MAC_WIFI_SOFTAP) );
        uint64_t some_val = (calcItemValue(mac_addr) % 100000L)*100000L; 
        ESP_LOGE(TAG1, "Setting systime to %llu - not booting from deepsleep", some_val);
        reset_systime(some_val); */
        printf("\n");
        ESP_LOGW(TAG0, "Waiting for start interrupt...");
        printf("\n");
        blockUntilCycle(INIT_DETECTION);
        startCycle();
        ESP_LOGW("MAIN","CONFIG: bc_duration_%d-TS_senddelay_%d-DS_duration_%d-ME_duration_%d ", CONFIG_BROADCAST_DURATION, CONFIG_TIMESTAMP_SEND_DELAY, CONFIG_DEEPSLEEP_DURATION_MS, CONFIG_MSG_EXCHANGE_DURATION_MS);
        printf("\n");
    }else{
        printf("\n");
        startCycle();
    }

    xTaskCreatePinnedToCore( network_reset_task     , "network_reset_task"      , 2048, NULL, 1, &network_reset_task_handle     , 1);
    xTaskCreatePinnedToCore( network_sync_task      , "network_sync_task"       , 4096, NULL, 1, &network_sync_task_handle      , 1);
    xTaskCreatePinnedToCore( neighbor_detection_task, "neighbor_detection_task" , 2048, NULL, 3, &neighbor_detection_task_handle, 1);
}
