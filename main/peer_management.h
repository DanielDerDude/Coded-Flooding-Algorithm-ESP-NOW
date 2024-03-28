#ifndef _INCLUDES_H_
#define _INCLUDES_H_
#include "includes.h"
#endif

#include "packetpool.h"

#define BRADCAST_COUNT_ESTIMATE     (CONFIG_BROADCAST_DURATION / CONFIG_TIMESTAMP_SEND_DELAY)

#define BLOOM_ERROR_RATE                        0.01    // estimated false positive rate of bloom fitlers used in  reception reports and virtual queue query
#define VIRTUAL_QUEUES_SIZE                     10      // size of outrput queue and all virtual queues
#define ACK_QUEUE_SIZE          MAX_PACKETS_IN_POOL

// offset type
typedef struct {
    int64_t avg_offset;                                // average offset
    int64_t offset_buffer[BRADCAST_COUNT_ESTIMATE];    // array for saving collected offests
    uint8_t idx;                                       // index pointing to next free element of buffer
} offset_data_t;

// xPeerElem type
typedef struct xPeerElem {
    uint8_t mac_add[ESP_NOW_ETH_ALEN];  	             // mac address of peer, list is sorted by mac address
    uint64_t item_value;                                 // item value numerical value of mac adrdress used for sorting list 
    offset_data_t* timing_data;                          // timing data
    QueueHandle_t ack_queue;                             // holds references to packets that need to be acked by the peer
    QueueHandle_t virtual_queue;                         // virtual queue of peer with poiters to packets in packet pool
    bloom_t* in_queue;                                   // bloom filter for checking if a packet is in the virtual queue
    bool has_sent_report;                                // flag if peer has sent his report - increases garbage collection efficiency
    struct xPeerElem* pxNext;                            // pointer pointing to next list entry
} xPeerElem_t;

// list type
typedef struct ListHandle {
    // PRIVATE: DO NOT MESS WITH THOSE, use get-functions instead
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

    // init ack queue
    newElem->ack_queue = xQueueCreate(ACK_QUEUE_SIZE, sizeof(PoolElem_t*));
    assert(newElem->ack_queue != NULL);

    // init virtual queue of peer
    newElem->virtual_queue = xQueueCreate(VIRTUAL_QUEUES_SIZE, sizeof(PoolElem_t*));
    assert(newElem->virtual_queue != NULL);

    // allocate memory and init reception report bloomfilter and check if successful
    newElem->in_queue = (bloom_t*)malloc(sizeof(bloom_t));
    assert(newElem->in_queue != NULL);

    // initialize bloom filter as reception report
    uint8_t ret = bloom_init2(newElem->in_queue, MAX_PACKETS_IN_POOL, BLOOM_ERROR_RATE);     // bloom filter will later be replaced with smaller size
    assert(ret == 0);

    newElem->has_sent_report = false;
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
    assert(list != NULL);
    xPeerElem_t* current = list->pxHead;
    xPeerElem_t* next;
    
    while (current != NULL) {
        next = current->pxNext;             // store next element
        free(current->timing_data);         
        bloom_free(current->in_queue);
        free(current->in_queue);
        vQueueDelete(current->ack_queue);
        vQueueDelete(current->virtual_queue);
        free(current);
        current = next;
    }

    memset(&list, 0, sizeof(list));
}

// adds a peer to the list, sorted by mac addr (high -> low)
void IRAM_ATTR vAddPeer(const uint8_t mac_addr[ESP_NOW_ETH_ALEN], int64_t new_offset) {
    assert(list != NULL);           //check if list exists
    
    // create peer list entry
    xPeerElem_t* newElem = xCreateElem(mac_addr, new_offset);
    
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
}

// adds an offset to a peer
void IRAM_ATTR vAddOffset(const uint8_t mac_addr[ESP_NOW_ETH_ALEN], int64_t new_offset){
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
}

// returns max offset of peer_list
int64_t IRAM_ATTR getMaxOffset(){
    uint64_t ret = list->max_offset;
    return ret;
}

// returns address of peer which holds the highest offset
void IRAM_ATTR getMaxOffsetAddr(uint8_t* addr_buff){
    if (list->peer_count == 0){
        ESP_ERROR_CHECK( esp_read_mac(addr_buff, ESP_MAC_TYPE) );
        return;
    }
    memcpy(addr_buff, &list->max_offset_addr, ESP_NOW_ETH_ALEN);
}

// retunrs number of peers in peerlist
uint8_t IRAM_ATTR getPeerCount(){
    uint8_t ret = list->peer_count;
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

static const char* TAG_RECREP = "recept_reort";

// returns a bloom filter as a reception report based on the current packet pool
bloom_t xCreateRecepRecepReport(){    
    uint8_t pkt_cnt = packet_pool.decoded_cnt;    // number packets that have been successfully decoded
    
    ESP_LOGE(TAG_RECREP, "Packets in reception report %d", pkt_cnt);

    bloom_t recep_report;
    uint8_t ret = bloom_init2(&recep_report, pkt_cnt, BLOOM_ERROR_RATE);   // initialize bloom filter as reception report
    assert(ret == 0);

    for(uint8_t i = 0; i < packet_pool.size; i++){                                  // go through every slot in hashmap
        if(packet_pool.hashtable[i].tag == DECODED){                                // check if packet was decoded
            uint32_t seq_num32 = (uint32_t) packet_pool.hashtable[i].pckt->seq_num; // current sequence number
            int8_t ret = bloom_add(&recep_report, &seq_num32, sizeof(uint32_t));    // add the sequence number to bloomfilter
            assert(ret != -1);
            vTagPacketInPeport(&packet_pool.hashtable[i]);                          // tag packet as used in reception report
        }
    }

    return recep_report;
}

// go through peer list and chekc if a reception reprot has been received from every peer
bool boAllPeersSentReports(){
    assert(list != NULL);
    assert(list->pxHead != NULL);
    bool ret = true;
    for (xPeerElem_t* ListElem = list->pxHead; ListElem != NULL; ListElem = ListElem->pxNext){      // go through list and check if evers flag is set
        if (!ListElem->has_sent_report) ret = false;
    }

    if (ret){                                                                                       // if all where set then go through list again and deassert flag
        for (xPeerElem_t* ListElem = list->pxHead; ListElem != NULL; ListElem = ListElem->pxNext) ListElem->has_sent_report = false;
    }
    
    return ret;
}

// checks if packet is in a reception report (also used for virtual queue)
bool boPacketInReport(bloom_t* recep_rep, uint16_t seq_num){
    assert(recep_rep != NULL);
    uint32_t seq_num32 = (uint32_t) seq_num;                            // internally used murmur hash needs buffer of at least 32 bit - wont work otherwise
    int8_t ret = bloom_check(recep_rep, &seq_num32, sizeof(uint32_t));     // check if paket in reception report
    assert(ret != -1);
    return ret == 1;
}

// parses the tagged packets in pool and checks if every peer received his encoded packets - tags packets which where acked as "in reception report"
void vCheckForRecommissions(const uint8_t mac_addr[ESP_NOW_ETH_ALEN], bloom_t* recep_rep){
    assert(list != NULL);
    assert(list->pxHead != NULL);
    assert(recep_rep != NULL);
    
    ESP_LOGW(TAG_POOL, "Checking if recommissions need to be made");
    
    uint8_t resend_cnt = 0;
    xPeerElem_t* ListElem = xFindElem(mac_addr);    // list entry of peer who just send his report
    assert(ListElem != NULL);
    ListElem->has_sent_report = true;               // set flag that this peer has sent his report

    PoolElem_t* PoolElem;
    while(xQueueReceive(ListElem->ack_queue, &PoolElem, 0) == pdTRUE){      // go through every packet which needs to be acked by the peer
        assert(PoolElem->pckt != NULL);
        if (boPacketInReport(recep_rep, PoolElem->pckt->seq_num)){
            vTagPacketInPeport(PoolElem);                                   // Tag packet as in reception report
        }else{
            BaseType_t ret = xQueueSend(ListElem->virtual_queue, &PoolElem, 0);     // place unacknowledged packet manually back into virtual queue
            assert(ret == pdTRUE);
            PoolElem->tag = UNTOUCHED;                                      // remove tags
            resend_cnt++;
        }
    }
    
    if (resend_cnt != 0) ESP_LOGE(TAG_POOL, "Scheduled %d retransmissions", resend_cnt);
}

// serializes a reception report as bloom filter into a byte stream
// already adds the packet type and crc - returns pointer to bytestream and writes size into ret_size
uint8_t* serialize_reception_report(bloom_t bloom, uint8_t* ret_size){
    assert(bloom.ready != 0);   

    uint8_t pckt_type = RECEP_REPORT;                                                                   // pacekt type

    uint16_t size = sizeof(pckt_type) + sizeof(bloom)-sizeof(bloom.bf) + bloom.bytes + sizeof(uint16_t);        // define and check size of bytestream 
    assert(size <= 255);

    uint8_t* bytestream = (uint8_t*) calloc(size, 1);
    bytestream[0] = pckt_type;                                                                      // copy packet type at beginning of bytestream
    memcpy(bytestream+sizeof(pckt_type), &bloom, sizeof(bloom)-sizeof(bloom.bf));                   // copy bloomfilter header
    memcpy(bytestream+sizeof(pckt_type)+sizeof(bloom)-sizeof(bloom.bf), bloom.bf, bloom.bytes);     // copy bloomfilter array
    uint16_t crc = esp_crc16_le(UINT16_MAX, bytestream, size-sizeof(uint16_t));                     // calculate crc

    memcpy(&bytestream[size - sizeof(crc)], &crc, sizeof(crc));               // copy crc to the end of the bytestream

    *ret_size = size;       // write size into ret_size size
    return bytestream;      // return bytestream
}

// parses serialized reception report into bloom filter
bloom_t parse_serialized_recep_report(uint8_t* bytestream, uint8_t size){
    assert(bytestream != NULL);
    assert(bytestream[0] == RECEP_REPORT);

    bloom_t bloom;                                                                  // init empty bloom struct
    uint8_t* bloomheader = bytestream + 1;                                          // pointing to bloom header in bytestream 
    uint8_t* bloomfilter = bytestream + 1 + sizeof(bloom) - sizeof(bloom.bf);    // pointing to bloom filter in bytestream

    memcpy(&bloom, bloomheader, sizeof(bloom) - sizeof(bloom.bf));              // copy bloom header into struct
    bloom.bf = (uint8_t*)calloc(bloom.bytes, 1);                                // allocate memory for bloomfilter
    assert(bloom.bf != NULL);
    memcpy(bloom.bf, bloomfilter, bloom.bytes);                                 // copy bloom filter from bytestream into allocated memory

    uint16_t crc_recv;
    memcpy(&crc_recv, &bytestream[size - sizeof(uint16_t)], sizeof(uint16_t));  // extract crc from bytestream

    uint16_t crc_calc = esp_crc16_le(UINT16_MAX, bytestream, size-sizeof(uint16_t));    // calculate crc16 over bloomfilter
    
    if (crc_calc != crc_recv){                                                          // compare checksums
        ESP_LOGE(TAG_RECREP, "Parsing reception report failed - invalid crc!");
        ESP_LOGE(TAG_RECREP, "crc_calc = %d", crc_calc);
        ESP_LOGE(TAG_RECREP, "crc_recv = %d", crc_recv);
        assert(1 == 0);
    }

    return bloom;
}

//////////////////// CODING FUNCTIONS ////////////////////

const char* TAG_CODING = "coding";

// parses the serialized onc data into the struct and frees data
void parse_serialized_onc_data(onc_data_t* parsed, uint8_t* data){
    parsed->type = data[0];
    assert(parsed->type = ONC_DATA);

    parsed->pckt_cnt = data[1];          // extract packet count
    
    parsed->pckt_id_array = (uint16_t*)calloc(sizeof(uint16_t), parsed->pckt_cnt);                           // allocate memory for pacekt id array - needs to be freed later on
    assert(parsed->pckt_id_array != NULL);

    memcpy(parsed->pckt_id_array, &data[2], sizeof(uint16_t)*parsed->pckt_cnt);                             // copy packet ids into id array field
    memcpy(parsed->encoded_data, &data[sizeof(uint16_t)*parsed->pckt_cnt + 2], sizeof(native_data_t));      // copy encoded data to struct

    free(data);
}

// refreshes virtual queues
void vRefreshVirtualQueues(PoolElem_t* PoolElem, const uint8_t mac_addr[ESP_NOW_ETH_ALEN]){
    assert(list->pxHead != NULL);
    uint16_t seq_num = PoolElem->pckt->seq_num;

    xPeerElem_t* ListElem = list->pxHead;
    while(ListElem != NULL){                                                        // go through each virtual queue of a peer
        if (uxQueueMessagesWaiting(ListElem->virtual_queue) == 0){                  // if queue is empty reset the "in queue" bloomfilter
            bloom_reset(ListElem->in_queue);
        }
        if (memcmp(ListElem->mac_add, mac_addr, ESP_NOW_ETH_ALEN) == 0) {                       // add sequence number to virtual queue of peer who sent the native packet
            uint32_t seq_num32 = (uint32_t)seq_num;                                             // packet sequence number
            int8_t ret_bloom = bloom_add(ListElem->in_queue, &seq_num32, sizeof(uint32_t));     // add the sequence number to "in queue" bloom filter
            assert(ret_bloom != -1);
        }else if (!boPacketInReport(ListElem->in_queue, seq_num)){                              // if not source peer but packet not in virtual queue - but funtion does the same
            BaseType_t ret_queue = xQueueSend(ListElem->virtual_queue, &PoolElem, 0);           // add reference to packet into the virtual queue
            assert(ret_queue == pdTRUE);

            uint32_t seq_num32 = (uint32_t)seq_num;                                             // packet sequence number
            int8_t ret_bloom = bloom_add(ListElem->in_queue, &seq_num32, sizeof(uint32_t));     // add the sequence number to "in queue" bloom filter
            assert(ret_bloom != -1);
            break;                                                                      // exit loop
        }
        ListElem = ListElem->pxNext;
    }
}

// returns the number of current packets that can be coded together
uint8_t xGetCodingCnt(){
    xPeerElem_t* ListElem = list->pxHead;

    uint8_t coding_cnt = 0;
    while((ListElem != NULL)){
        if (uxQueueMessagesWaiting(ListElem->virtual_queue) > 0){                   // if queue is not empty
            coding_cnt++;
        }else{
            bloom_reset(ListElem->in_queue);    // if queue is empty reset the "in queue" bloomfilter
        }
        ListElem = ListElem->pxNext;
    }
    return coding_cnt;
}

// goes through all virtual queues and encodes packets if their not the same - !!! allocated memory for packet id array needs to be freed
onc_data_t xGenerateCodedPaket(){
    onc_data_t enc_pckt = {0};
    enc_pckt.type = ONC_DATA;

    xPeerElem_t* ListElem = list->pxHead;

    uint16_t id_buff[getPeerCount()];
    uint8_t pckt_cnt = 0;
    char str_buff[6*getPeerCount() + 1];
    str_buff[0] = '\0';

    while((ListElem != NULL)){                                                              // itearte over virtual queues
        if (uxQueueMessagesWaiting(ListElem->virtual_queue) > 0){                           // if current virtual queue not empty
            PoolElem_t* PoolElement; 
            if (xQueueReceive(ListElem->virtual_queue, &PoolElement, 0) == pdTRUE){         // get packet from virtual queue
                assert(PoolElement != NULL);
                if (PoolElement->pckt == NULL) continue;                                    // if packet is already gone take next packet or skip this virtual queue
                
                id_buff[pckt_cnt] = PoolElement->pckt->seq_num;                             // add sequence number of native packet to packet id list of onc packet 
                pckt_cnt++;                                                                 // increase packet count used for encoding

                uint8_t* bytes = (uint8_t*)(PoolElement->pckt);                             // XOR packet bitwise from virtual queue into onc data field
                for(uint8_t i = 0; i < sizeof(native_data_t); i++){
                    enc_pckt.encoded_data[i] ^= bytes[i];
                }
                
                BaseType_t ret = xQueueSend(ListElem->ack_queue, &PoolElement, 0);          // add the encoded packet to the ack queue of the peer
                assert(ret == pdTRUE);

                char temp[7];
                sprintf(temp, "%5d ", PoolElement->pckt->seq_num);                          // save string of packet id in string list for later logging
                strcat(str_buff, temp);
            }else assert(1 == 0);       // this should not trigger
        }else{
            bloom_reset(ListElem->in_queue);                                                // if queue is empty reset the "in queue" bloomfilter
        }
        ListElem = ListElem->pxNext;
    }

    // fill struct
    enc_pckt.pckt_cnt = pckt_cnt;
    enc_pckt.pckt_id_array = (uint16_t*)calloc(pckt_cnt, sizeof(uint16_t));
    memcpy(enc_pckt.pckt_id_array, id_buff, pckt_cnt*sizeof(uint16_t));

    ESP_LOGW(TAG_CODING, "Encoded packets [ %s]", str_buff);

    return enc_pckt;
}

// returns true if decoding was successfull and writes decoded packet into delPckt
bool boDecodePacket(native_data_t* decPckt, onc_data_t* encPckt){
    assert(encPckt->type == ONC_DATA);
    assert(decPckt != NULL);
    assert(encPckt != NULL);

    char str_buff[6*getPeerCount() + 1];
    str_buff[0] = '\0';

    for(uint8_t i = 0; i < encPckt->pckt_cnt; i++){         // read sequence enumbers from packet and as save string
        char temp[7];
        sprintf(temp, "%5d ", encPckt->pckt_id_array[i]);         
        strcat(str_buff, temp);
    }

    memcpy(decPckt, &encPckt->encoded_data, sizeof(native_data_t));      // copy encoded data
    uint8_t* decPckt_bytes = (uint8_t*) decPckt;

    uint8_t missing_cnt = 0;
    char str_missing[6*getPeerCount()+1];       // string buffer for missing packets
    str_missing[0] = '\0';

    for (uint8_t i = 0; i < encPckt->pckt_cnt; i++){                                    // find every packet used for encoding in packet pool and XOR it with encoded packet
        native_data_t* pckt = xPacketPoolGetPacket(encPckt->pckt_id_array[i]);          // get packet from packet pool (registers coding tag internally)
        if(pckt != NULL){                                                               // if packet lookup successfull
            uint8_t* temp_bytes = (uint8_t*)pckt;                                       // XOR packet bitwise from virtual queue with encoded onc data field
            for(uint8_t j = 0; j < sizeof(native_data_t); j++){
                decPckt_bytes[j] ^= temp_bytes[j];
            }
        }else{
            missing_cnt++;                                              // increase missing packet count
            char temp[7];
            sprintf(temp, "%5d ", encPckt->pckt_id_array[i]);           // save string of missing packet id in string buffer
            strcat(str_missing, temp);
        }
    }

    if (missing_cnt > 1){                                                               // decode failed - more than one packet is missing
        ESP_LOGE(TAG_CODING, "Decoding failed - missing packets [ %s]", str_missing);
        return false;
    }

    if (decPckt->type != NATIVE_DATA){                                                  // decode failed if type not correct
        ESP_LOGE(TAG_CODING, "Decoding failed - undefined type %d", decPckt->type);
        return false;
    }

    uint16_t crc16_calc = esp_crc16_le(UINT16_MAX, decPckt->payload, PAYLOAD_LEN*sizeof(uint8_t));      // calculate crc16 of decoded packet
    if (decPckt->crc16 != crc16_calc){
        ESP_LOGE(TAG_CODING, "Decoding failed - invalid checksum");
        return false;                                                                                   // and compare with crc in crc field
    }

    ESP_LOGW(TAG_CODING, "Decoded packet %d from [ %s]", decPckt->seq_num, str_buff);
    return true;
}