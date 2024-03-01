#ifndef _INCLUDES_H_
#define _INCLUDES_H_
#include "includes.h"
#include "./components/bloomfilter/bloom.c"
#endif

#include "packetpool.h"

#define BRADCAST_COUNT_ESTIMATE     (CONFIG_BROADCAST_DURATION / CONFIG_TIMESTAMP_SEND_DELAY)

#define BLOOM_MAX_ENTRIES     150     // estimated maximum entries the reception report can hold 
#define BLOOM_ERROR_RATE      0.01    // estimated false positive rate of reception reports 

#define CODING_QUEUES_SIZE    20      // size of outrput queue and all virtual queues

// offset type
typedef struct {
    int64_t avg_offset;                                // average offset
    int64_t offset_buffer[BRADCAST_COUNT_ESTIMATE];    // array for saving collected offests
    uint8_t idx;                                       // index pointing to next free element of buffer
} offset_data_t;

// xPeerElem type
typedef struct xPeerElem {
    uint8_t mac_add[ESP_NOW_ETH_ALEN];  	             // mac address of peer, list is sorted by mac address
    uint64_t item_value;
    offset_data_t* timing_data;                          // timing data
    bloom_t* recp_report;                                // pointer to reception report in form of a bloom filter 
    QueueHandle_t virtual_queue;                         // "virtual" queue of peer - not really virtual.. this will take storage
    struct xPeerElem* pxNext;                            // pointer pointing to next list entry
} xPeerElem_t;

// list type
typedef struct ListHandle {
    // PRIVATE: DO NOT MESS WITH THOSE, use get-functions instead  ( )
    int64_t max_offset;                          // max offset in list relative to current systime (send offset not accounted)
    int64_t max_offset_addr[ESP_NOW_ETH_ALEN];   // mac addres of peer with highest offset
    uint8_t peer_count;                          // number of peers in list, excluding the broadcast peer
    xPeerElem_t* pxHead;                         // points to first peer Elem in list
    xPeerElem_t* pxTail;                         // points to last peer Elem in list
} PeerListHandle_t;

static PeerListHandle_t* list;

//////////////////// PRIVATE UTILITY FUNCTIONS  ////////////////////

// PRIVATE: computes item value
static uint64_t IRAM_ATTR calcItemValue(const uint8_t mac_addr[ESP_NOW_ETH_ALEN]){
    uint64_t item_value = 0;
    for (int i = 0; i < ESP_NOW_ETH_ALEN; i++) {
        item_value = (item_value << 8) | mac_addr[i];
    }
    return item_value;
}

// PRIVATE: returns pointer to newly created Elem
static xPeerElem_t* IRAM_ATTR xCreateElem(const uint8_t mac_addr[ESP_NOW_ETH_ALEN], int64_t first_offset){
    // allocate memory for new Elem
    xPeerElem_t* newElem = (xPeerElem_t*)malloc(sizeof(xPeerElem_t));
    assert(newElem != NULL);
    memset(newElem, 0, sizeof(xPeerElem_t));
    // copy mac addres
    memcpy(newElem->mac_add, mac_addr, ESP_NOW_ETH_ALEN);

    // allocate memory for offset data
    offset_data_t* timing_data = (offset_data_t*)malloc(sizeof(offset_data_t));
    assert(timing_data != NULL);
    memset(timing_data, 0, sizeof(offset_data_t));
    
    // set avg_offset to first offset and add first offset to buffer, also init buffer index
    timing_data->avg_offset = first_offset;
    timing_data->offset_buffer[0] = first_offset;
    timing_data->idx = 1;
    
    newElem->timing_data = timing_data;
    newElem->item_value = calcItemValue(mac_addr);

    // init pointers
    newElem->pxNext = NULL;

    // init virtual queue of peer
    newElem->virtual_queue = xQueueCreate(CODING_QUEUES_SIZE, sizeof(native_data_t));
    assert(newElem->virtual_queue != NULL);   

    // allocate memory and init reception report bloomfilter and check if successful
    newElem->recp_report = (bloom_t*)malloc(sizeof(bloom_t));
    uint8_t ret = bloom_init2(newElem->recp_report, BLOOM_MAX_ENTRIES, BLOOM_ERROR_RATE);
    assert(ret == 0);

    return newElem;
}

// PRIVATE: iterates through list and finds the list element of a mac address
static xPeerElem_t* IRAM_ATTR xFindElem(const uint8_t mac_addr[ESP_NOW_ETH_ALEN]){
    // iterate through list and find item Value
    uint64_t searchedValue = calcItemValue(mac_addr);

    xPeerElem_t* ListElem = list->pxHead;
    
    while((ListElem != NULL) && (ListElem->item_value != searchedValue)){
        ListElem = ListElem->pxNext;
    }
    return ListElem;
}

//////////////////// PEER AND TIMING DATA COLLECTION  ////////////////////

// function to initialise the peer list
void vInitPeerList() {
    assert(list == NULL);                                            // this triggers if init function called twice
    list = (PeerListHandle_t*) malloc(sizeof(PeerListHandle_t));
    assert(list != NULL);
    memset(list, 0, sizeof(PeerListHandle_t));

    //newList->sem = //xSemaphoreCreateMutex();
    list->max_offset = 0;
    list->peer_count = 0;
    list->pxHead = NULL;
    list->pxTail = NULL;
    
    return;
}

// deletes the peer list and frees theallocated memory
void vDeletePeerList(){
    //xSemaphoreTake(list->sem, portMAX_DELAY);
    assert(list != NULL);
    xPeerElem_t* current = list->pxHead;
    xPeerElem_t* next;
    
    while (current != NULL) {
        next = current->pxNext;
        free(current->timing_data);
        bloom_free(current->recp_report);
        free(current->recp_report);
        free(current);
        current = next;
    }

    //xSemaphoreGive(list->sem);
    //vSemaphoreDelete(list->sem);
    memset(&list, 0, sizeof(list));
    //free(list);
}

// adds a peer to the list, sorted by mac addr (high -> low)
void IRAM_ATTR vAddPeer(const uint8_t mac_addr[ESP_NOW_ETH_ALEN], int64_t new_offset) {
    //xSemaphoreTake(list->sem, portMAX_DELAY);
    assert(list != NULL);           //check if list exists
    
    // create peer list entry
    xPeerElem_t* newElem = xCreateElem(mac_addr, new_offset);
    //ESP_LOGW("LIST", "vAddPeer for "MACSTR" with itemValue %llu", MAC2STR(mac_addr), newElem->item_value);
    
    // insert element in list
    if (list->pxHead == NULL && list->pxTail == NULL ){      // if list is empty
        list->pxHead = newElem;
        list->pxTail = newElem;
    } else { 
        // iterate list and find correct position for new element
        xPeerElem_t* current = list->pxHead;
        while (current != NULL && current->item_value < newElem->item_value) {    
            current = current->pxNext;
        }

        // insert element
        if (current == NULL) {                  // position at the end of list
            //newElem->pxPrev = list->pxTail;
            list->pxTail->pxNext = newElem;
            list->pxTail = newElem;
        } else if (current == list->pxHead) {   // position at the beginning of the list
            newElem->pxNext = list->pxHead;
            list->pxHead = newElem;
        } else {                                // inbetween the list
            newElem->pxNext = current;
        }
    }

    // give peer to esp now API
    esp_now_peer_info_t *peer = (esp_now_peer_info_t*)malloc(sizeof(esp_now_peer_info_t));
    assert(peer != NULL);

    memset(peer, 0, sizeof(esp_now_peer_info_t));
    peer->channel = CONFIG_ESPNOW_CHANNEL;
    peer->ifidx = ESPNOW_WIFI_IF;
    peer->encrypt = true;
    memcpy(peer->lmk, CONFIG_ESPNOW_LMK, ESP_NOW_KEY_LEN);
    memcpy(peer->peer_addr, mac_addr, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK( esp_now_add_peer(peer) );
    free(peer);

    // increase peer count
    list->peer_count++;

    // check if new offset is highest offset
    if ((new_offset > list->max_offset)){                   // if first element in list or offset is max offset
        list->max_offset = new_offset;                              // set new max offset
        memcpy(list->max_offset_addr, mac_addr, ESP_NOW_ETH_ALEN);  // copy mac address
    }
    //xSemaphoreGive(list->sem);
}

// adds an offset to a peer
void IRAM_ATTR vAddOffset(const uint8_t mac_addr[ESP_NOW_ETH_ALEN], int64_t new_offset){
    //xSemaphoreTake(list->sem, portMAX_DELAY);
    
    assert(list != NULL);                                       // assert that list exists
    assert((list->pxHead != NULL));                             // assert that at least one element exists
    
    xPeerElem_t* ListElem = xFindElem(mac_addr);
    assert(ListElem != NULL);

    // add new offset to buffer and increase buffer index
    offset_data_t* timing_data = ListElem->timing_data;
    timing_data->offset_buffer[timing_data->idx] = new_offset;
    timing_data->idx++;
    
    // compute new average offset
    int64_t temp = 0;
    for (uint8_t i = 0; i < timing_data->idx; i++){
        temp += timing_data->offset_buffer[i];
    }
    timing_data->avg_offset = temp / timing_data->idx;

    // check if new computed offset of peer is max offset
    if (timing_data->avg_offset > list->max_offset){
        list->max_offset = timing_data->avg_offset;         // set new max offset
        memcpy(list->max_offset_addr, mac_addr, ESP_NOW_ETH_ALEN);  // copy mac address
    }
    //xSemaphoreGive(list->sem);
}

// returns max offset of peer_list
int64_t IRAM_ATTR getMaxOffset(){
    //xSemaphoreTake(list->sem, portMAX_DELAY);
    uint64_t ret = list->max_offset;
    //xSemaphoreGive(list->sem);
    return ret;
}

// returns address of peer which holds the highest offset
void IRAM_ATTR getMaxOffsetAddr(uint8_t* addr_buff){
    //xSemaphoreTake(list->sem, portMAX_DELAY);
    if (list->peer_count == 0){
        ESP_ERROR_CHECK( esp_read_mac(addr_buff, ESP_MAC_WIFI_SOFTAP) );
        return;
    }
    memcpy(addr_buff, list->max_offset_addr, ESP_NOW_ETH_ALEN);
    //xSemaphoreGive(list->sem);
}

// retunrs number of peers in peerlist
uint8_t IRAM_ATTR getPeerCount(){
    //xSemaphoreTake(list->sem, portMAX_DELAY);
    uint8_t ret = list->peer_count;
    //xSemaphoreGive(list->sem);
    return ret;
}

// returns true if peer exists in the list
bool boPeerExists(const uint8_t mac_addr[ESP_NOW_ETH_ALEN]){
    xPeerElem_t* ListElem = xFindElem(mac_addr);
    bool list_found = (ListElem != NULL);
    bool api_found = esp_now_is_peer_exist(mac_addr);
    return (api_found && list_found);
}


//////////////////// RECEPTION REPORT FUNCTIONS ////////////////////

// adds a new packet id to the reception report of a peer in peer list
void IRAM_ATTR vAddReception(const uint8_t mac_addr[ESP_NOW_ETH_ALEN], uint32_t newPacketID){
    //xSemaphoreTake(list->sem, portMAX_DELAY);
    
    assert(list != NULL);                                       // assert that list exists
    assert((list->pxHead != NULL));                             // assert that at least one element exists
    
    xPeerElem_t* ListElem = xFindElem(mac_addr);

    int8_t ret = bloom_add(ListElem->recp_report, &newPacketID, sizeof(uint32_t));
    assert(ret != -1);
    
    //xSemaphoreGive(list->sem);
}

// returns true if a paket with the sequence number is in the reception report 
bool boPaketInReport(const uint8_t mac_addr[ESP_NOW_ETH_ALEN], const uint16_t paket_seq_num){
    assert(list != NULL);                                       // assert that list exists
    assert((list->pxHead != NULL));                             // assert that at least one element exists
    xPeerElem_t* ListElem = xFindElem(mac_addr);
    assert(ListElem != NULL);

    int8_t ret = bloom_check(ListElem->recp_report, (uint8_t*)&paket_seq_num, sizeof(uint16_t)); // check if paket in reception report
    assert(ret != -1);

    return ret == 1;
}

// replaces the old reception report of a peer with a new one
// newReport argument does not need to be valid after function returns
void IRAM_ATTR vReplaceReport(const uint8_t mac_addr[ESP_NOW_ETH_ALEN], bloom_t* newReport){
    //xSemaphoreTake(list->sem, portMAX_DELAY);
    assert(list != NULL);                                       // assert that list exists
    assert((list->pxHead != NULL));                             // assert that at least one element exists
    
    xPeerElem_t* ListElem = xFindElem(mac_addr);          // find list element with corresponding mac address

    int8_t ret = bloom_reset(ListElem->recp_report);            // reset old reception report
    assert(ret == 0);
    ret = bloom_merge(ListElem->recp_report, newReport);    // merge new reception report into old packet 
    assert(ret == 0);
    //xSemaphoreGive(list->sem);
}

// replaces the old reception report of a list element with a new one
// so newReport argument does not need to be valid after function returns
void IRAM_ATTR vMergeReports(const uint8_t mac_addr[ESP_NOW_ETH_ALEN], bloom_t* newReport){
    //xSemaphoreTake(list->sem, portMAX_DELAY);
    assert(list != NULL);                                       // assert that list exists
    assert((list->pxHead != NULL));                             // assert that at least one element exists
    
    xPeerElem_t* ListElem = xFindElem(mac_addr);          // find list element with corresponding mac address

    int8_t ret = bloom_merge(ListElem->recp_report, newReport);
    assert(ret == 0);
    //xSemaphoreGive(list->sem);
}


//////////////////// CODING FUNCTIONS ////////////////////

// packet handler for native paket 
void vRefreshVirtualQueues(const native_data_t* newPacket){
    // add packet to packet pool if in already
    vPacketPoolAdd(newPacket);

    // refresh virtual queues  
    xPeerElem_t* ListElem = list->pxHead;
    bool hit = false;

    while((ListElem != NULL)){                                                      // go through each virtual queue of a peer
        if (!boPaketInReport(ListElem->mac_add, newPacket->seq_num)){                // if a peer does not have this packet
            BaseType_t ret = xQueueSend(ListElem->virtual_queue, &newPacket, 0);    // add it to the virtual queue and exit loop
            assert(ret == pdTRUE);
            hit = true;                                                             // flag that at least one peer does not 
            break;
        }
        ListElem = ListElem->pxNext;
    }

    if(!hit) xPacketPoolRemove(newPacket->seq_num);                                  // if every peer has that packet, remove it from packet pool
}

// goes through all virtual queues and encodes packets if their not the same - !!! allocated memory for packet id array needs to be freed
onc_data_t xGenerateCodedPaket(){
    onc_data_t enc_pckt;
    enc_pckt.type = ONC_DATA;

    xPeerElem_t* ListElem = list->pxHead;

    uint16_t id_buff[getPeerCount()];  
    uint8_t pckt_cnt = 0;

    while((ListElem != NULL)){
        if (uxQueueMessagesWaiting(ListElem->virtual_queue) > 0){                   // if queue is not empty
            native_data_t pckt_buff;
            assert(pckt_buff.type == NATIVE_DATA);                                  // if this is not true something is terribly wrong 
            if (xQueueReceive(ListElem->virtual_queue, &pckt_buff, 0) == pdTRUE){   // get packet from virtual queue
                id_buff[pckt_cnt] = pckt_buff.seq_num;                              // add sequence number of native packet to packet id list of onc packet 
                pckt_cnt++;                                                         // increase packet count of used encoded packet

                uint8_t* bytes = (uint8_t*)&pckt_buff;                              // XOR packet bitwise from virtual queue into onc data field
                for(uint8_t i = 0; i < sizeof(native_data_t); i++){
                    enc_pckt.encoded_data[i] ^= bytes[i];
                }
            }
        }
        ListElem = ListElem->pxNext;
    }
    
    enc_pckt.pckt_cnt = pckt_cnt;
    enc_pckt.pckt_id_array = (uint16_t*)calloc(pckt_cnt, sizeof(uint16_t));

    return enc_pckt;
}

// returns true if decoding was successfull and writes decoded packet into decPckt
bool boDecodePacket(native_data_t* decPckt, onc_data_t* encPckt){
    assert(encPckt->type == ONC_DATA);

    memcpy(decPckt, encPckt->encoded_data, sizeof(native_data_t));      // copy encoded packet
    uint8_t* decPckt_bytes = (uint8_t*) decPckt;

    for (uint8_t i = 0; i < encPckt->pckt_cnt; i++){                                    // find every packet used for encoding in packet pool and XOR it with encoded packet 
        native_data_t temp;
        if(boPacketPoolGet(&temp, encPckt->pckt_id_array[i]) == false) return false;    // decode failed if packet not in packet pool
        
        uint8_t* temp_bytes = (uint8_t*)&temp;                                          // XOR packet bitwise from virtual queue with encoded onc data field
        for(uint8_t j = 0; j < sizeof(native_data_t); j++){
            decPckt_bytes[j] ^= temp_bytes[j];
        }
    }

    if (decPckt->type != NATIVE_DATA) return false;                                                     // decode failed if type not correct

    uint16_t crc16_calc = esp_crc16_le(UINT16_MAX, decPckt->payload, PAYLOAD_LEN*sizeof(uint8_t));      // calculate crc16 of decoded packet
    if (decPckt->crc16 != crc16_calc) return false;                                                     // and compare with crc in crc field

    return true;
}
