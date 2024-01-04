#ifndef _INCLUDES_H_
#define _INCLUDES_H_
#include "includes.h"
#include "./components/bloomfilter/bloom.c"
#endif

#define BRADCAST_COUNT_ESTIMATE     (CONFIG_BROADCAST_DURATION / CONFIG_TIMESTAMP_SEND_DELAY)

#define BLOOM_MAX_ENTRIES     150     // estimated maximum entries the reception report can hold 
#define BLOOM_ERROR_RATE      0.01    // estimated false positive rate of reception reports 

// offset type
typedef struct {
    int64_t avg_offset;                                 // average offset
    int64_t offset_buffer[BRADCAST_COUNT_ESTIMATE];    // array for saving collected offests
    uint8_t idx;                                        // index pointing to next free element of buffer
} offset_data_t;

// xPeerElem type
typedef struct xPeerElem {
    uint8_t mac_add[ESP_NOW_ETH_ALEN];  	             // mac address of peer, list is sorted by mac address
    offset_data_t* timing_data;                          // timing data
    uint64_t item_value;
    struct xPeerElem* pxNext;                            // pointer pointing to next list entry
    //struct xPeerElem* pxPrev;                            // pointer pointing to previous list entry
    bloom_t* recp_report;                                // pointer to reception report in form of a bloom filter 
} xPeerElem_t;

// list type
typedef struct ListHandle {
    // PRIVATE: DO NOT MESS WITH THOSE, use get-functions instead  ( )
    //SemaphoreHandle_t sem;
    int64_t max_offset;                         // max offset in list relative to current systime (send offset not accounted)
    int64_t max_offset_addr[ESP_NOW_ETH_ALEN];  // mac addres of peer with highest offset
    uint8_t peer_count;                         // number of peers in list, excluding the broadcast peer
    xPeerElem_t* pxHead;                        // points to first peer Elem in list
    xPeerElem_t* pxTail;                        // points to last peer Elem in list
} PeerListHandle_t;


// computes item value
static uint64_t IRAM_ATTR calcItemValue(const uint8_t mac_addr[ESP_NOW_ETH_ALEN]){
    uint64_t item_value = 0;
    for (int i = 0; i < ESP_NOW_ETH_ALEN; i++) {
        item_value = (item_value << 8) | mac_addr[i];
    }
    return item_value;
}

// returns pointer to newly created Elem
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
    //newElem->pxPrev = NULL;

    // allocate memory and init reception report bloomfilter and check if successful
    newElem->recp_report = (bloom_t*)malloc(sizeof(bloom_t));
    uint8_t ret = bloom_init2(newElem->recp_report, BLOOM_MAX_ENTRIES, BLOOM_ERROR_RATE);
    assert(ret == 0);

    return newElem;
}

// iterates through list and finds the list element of a mac address
static xPeerElem_t* IRAM_ATTR xFindElem(PeerListHandle_t* list, const uint8_t mac_addr[ESP_NOW_ETH_ALEN]){
    // iterate through list and find item Value
    uint64_t searchedValue = calcItemValue(mac_addr);

    xPeerElem_t* ListElem = list->pxHead;
    
    while((ListElem != NULL) && (ListElem->item_value != searchedValue)){
        ListElem = ListElem->pxNext;
    }
    assert(ListElem != NULL);
    return ListElem;
}

/* Function to initialise the List, returns pointer to list Handle */
PeerListHandle_t* xInitPeerList() {
    PeerListHandle_t* newList = (PeerListHandle_t*) malloc(sizeof(PeerListHandle_t));
    assert(newList != NULL);
    memset(newList, 0, sizeof(PeerListHandle_t));

    //newList->sem = //xSemaphoreCreateMutex();
    newList->max_offset = 0;
    newList->peer_count = 0;
    newList->pxHead = NULL;
    newList->pxTail = NULL;
    
    return newList;
}

// adds a peer to the list, sorted by mac addr (high -> low)
void IRAM_ATTR vAddPeer(PeerListHandle_t* list, const uint8_t mac_addr[ESP_NOW_ETH_ALEN], int64_t new_offset) {
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
            //list->pxHead->pxPrev = newElem;  
            list->pxHead = newElem;
        } else {                                // inbetween the list
            //newElem->pxPrev = current->pxPrev;
            newElem->pxNext = current;
            //current->pxPrev->pxNext = newElem;
            //current->pxPrev = newElem;
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
void IRAM_ATTR vAddOffset(PeerListHandle_t* list, const uint8_t mac_addr[ESP_NOW_ETH_ALEN], int64_t new_offset){
    //xSemaphoreTake(list->sem, portMAX_DELAY);
    
    assert(list != NULL);                                       // assert that list exists
    assert((list->pxHead != NULL));                             // assert that at least one element exists
    
    xPeerElem_t* ListElem = xFindElem(list, mac_addr);
    if (ListElem == NULL) return;

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
int64_t IRAM_ATTR getMaxOffset(PeerListHandle_t* list){
    //xSemaphoreTake(list->sem, portMAX_DELAY);
    uint64_t ret = list->max_offset;
    //xSemaphoreGive(list->sem);
    return ret;
}

// returns address of peer which holds the highest offset
void IRAM_ATTR getMaxOffsetAddr(PeerListHandle_t* list, uint8_t* addr_buff){
    //xSemaphoreTake(list->sem, portMAX_DELAY);
    if (list->peer_count == 0){
        ESP_ERROR_CHECK( esp_read_mac(addr_buff, ESP_MAC_WIFI_SOFTAP) );
        return;
    }
    memcpy(addr_buff, list->max_offset_addr, ESP_NOW_ETH_ALEN);
    //xSemaphoreGive(list->sem);
}

// retunrs number of peers in peerlist
uint8_t IRAM_ATTR getPeerCount(PeerListHandle_t* list){
    //xSemaphoreTake(list->sem, portMAX_DELAY);
    uint8_t ret = list->peer_count;
    //xSemaphoreGive(list->sem);
    return ret;
}

// adds a new packet id to the reception report of a peer in peer list
void IRAM_ATTR vAddReception(PeerListHandle_t* list, const uint8_t mac_addr[ESP_NOW_ETH_ALEN], uint32_t newPacketID){
    //xSemaphoreTake(list->sem, portMAX_DELAY);
    
    assert(list != NULL);                                       // assert that list exists
    assert((list->pxHead != NULL));                             // assert that at least one element exists
    
    xPeerElem_t* ListElem = xFindElem(list, mac_addr);

    int8_t ret = bloom_add(ListElem->recp_report, &newPacketID, sizeof(uint32_t));
    assert(ret != -1);
    
    //xSemaphoreGive(list->sem);
}

// replaces the old reception report of a peer with a new one
// newReport argument does not need to be valid after function returns
void IRAM_ATTR vReplaceReport(PeerListHandle_t* list, const uint8_t mac_addr[ESP_NOW_ETH_ALEN], bloom_t* newReport){
    //xSemaphoreTake(list->sem, portMAX_DELAY);
    assert(list != NULL);                                       // assert that list exists
    assert((list->pxHead != NULL));                             // assert that at least one element exists
    
    xPeerElem_t* ListElem = xFindElem(list, mac_addr);          // find list element with corresponding mac address

    int8_t ret = bloom_reset(ListElem->recp_report);            // reset old reception report
    assert(ret == 0);
    ret = bloom_merge(ListElem->recp_report, newReport);    // merge new reception report into old packet 
    assert(ret == 0);
    //xSemaphoreGive(list->sem);
}

// replaces the old reception report of a list element with a new one
// so newReport argument does not need to be valid after function returns
void IRAM_ATTR vMergeReports(PeerListHandle_t* list, const uint8_t mac_addr[ESP_NOW_ETH_ALEN], bloom_t* newReport){
    //xSemaphoreTake(list->sem, portMAX_DELAY);
    assert(list != NULL);                                       // assert that list exists
    assert((list->pxHead != NULL));                             // assert that at least one element exists
    
    xPeerElem_t* ListElem = xFindElem(list, mac_addr);          // find list element with corresponding mac address

    int8_t ret = bloom_merge(ListElem->recp_report, newReport);
    assert(ret == 0);
    //xSemaphoreGive(list->sem);
}

void vDeletePeerList(PeerListHandle_t* list){
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
    free(list);
}