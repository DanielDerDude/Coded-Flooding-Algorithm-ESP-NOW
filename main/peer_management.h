#ifndef _INCLUDES_H_
#define _INCLUDES_H_
#include "includes.h"
#endif

#include "packetpool.h"

#define BRADCAST_COUNT_ESTIMATE     (CONFIG_NEIGHBOR_DETECTION_DURATION / CONFIG_TIMESTAMP_SEND_DELAY)

#define BLOOM_ERROR_RATE                        0.01    // estimated false positive rate of bloom fitlers used in  reception reports and virtual queue query
#define VIRTUAL_QUEUES_SIZE                     30      // size of all virtual queues
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
    QueueHandle_t ack_queue;                             // holds sequence numbers of packets which need to be acked by the peer
    QueueHandle_t virtual_queue;                         // virtual queue of peer with poiters to packets in packet pool
    bool virtHeadRound;                                  // flag to separate packets in virtual queue heads into different coding rounds 
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
    struct xPeerElem* pxPrio;                    // coding iterator points to prioritised peer in coding round - changes in round robin fashing for each coding round
    QueueHandle_t onc_queue;                     // opportunistic cash queue for packets which where not decoded
    QueueHandle_t retrans_queue;                 // queue for retransmissions - this is prioretised when coding
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
    newElem->ack_queue = xQueueCreate(ACK_QUEUE_SIZE, sizeof(uint16_t));
    assert(newElem->ack_queue != NULL);

    // init virtual queue and queue flag of peer
    newElem->virtual_queue = xQueueCreate(VIRTUAL_QUEUES_SIZE, sizeof(uint16_t));
    assert(newElem->virtual_queue != NULL);
    newElem->virtHeadRound = false;

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
    
    list->onc_queue = xQueueCreate(VIRTUAL_QUEUES_SIZE, sizeof(onc_data_t*));
    list->retrans_queue = xQueueCreate(ACK_QUEUE_SIZE, sizeof(uint16_t));
}

// deletes the peer list and frees theallocated memory
void vDeletePeerList(){
    assert(list != NULL);
    xPeerElem_t* current = list->pxHead;
    xPeerElem_t* next;
    
    while (current != NULL) {
        next = current->pxNext;             // save next element and delete the current one
        free(current->timing_data);         
        vQueueDelete(current->ack_queue);
        vQueueDelete(current->virtual_queue);
        free(current);
        current = next;
    }
    vQueueDelete(list->onc_queue);
    vQueueDelete(list->retrans_queue);

    memset(&list, 0, sizeof(list));
}

// adds a peer to the list, sorted by mac addr (high -> low)
void IRAM_ATTR vAddPeer(const uint8_t mac_addr[ESP_NOW_ETH_ALEN], int64_t new_offset) {
    assert(list != NULL);           //check if list exists
    
    // create peer list entry
    xPeerElem_t* newElem = xCreateElem(mac_addr, new_offset);
    
    // insert element in list
    if (list->pxHead == NULL){      // if list is empty
        list->pxHead = newElem;
        list->pxTail = newElem;
        list->pxPrio = newElem;   // point coding header to head
    } else { 
        // iterate list and find correct position for new element
        xPeerElem_t* current = list->pxHead;
        xPeerElem_t* prev = list->pxHead;
        while (current != NULL) {
            if (current->item_value < newElem->item_value) break;
            prev = current;
            current = current->pxNext;
        }

        // insert element
        if (current == NULL) {                  // position at the end of list
            list->pxTail->pxNext = newElem;
            list->pxTail = newElem;
        } else if (current == list->pxHead) {   // position at the beginning of the list
            newElem->pxNext = list->pxHead;
            list->pxHead = newElem;
            list->pxPrio = newElem;           // point coding header to head
        } else {                                // inbetween the list
            prev->pxNext = newElem;
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
        }else if(packet_pool.hashtable[i].tag == NATIVE){                           // delete old native as in reception report
            vTagPacketInPeport(&packet_pool.hashtable[i]);                          // their not really in the report but also need to be garbage collected
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

// checks if sequence number is in a reception report (also used for virtual queues)
bool boPacketInReport(bloom_t* recep_rep, uint16_t seq_num){
    assert(recep_rep != NULL);
    uint32_t seq_num32 = (uint32_t) seq_num;                                // internally used murmur hash needs buffer of at least 32 bit - wont work otherwise
    int8_t ret = bloom_check(recep_rep, &seq_num32, sizeof(uint32_t));      // check if paket in reception report
    assert(ret != -1);
    return ret == 1;
}

// parses reception report of a peer and 
void vCheckForRetransmissions(const uint8_t mac_addr[ESP_NOW_ETH_ALEN], bloom_t* recep_rep){
    assert(list != NULL);
    assert(list->pxHead != NULL);
    assert(recep_rep != NULL);
    
    xPeerElem_t* ListElem = xFindElem(mac_addr);    // list entry of peer who just send his report
    assert(ListElem != NULL);
    ListElem->has_sent_report = true;               // set flag that this peer has sent his report

    uint16_t ack_seqnum = 0;
    uint8_t ack_cnt = uxQueueMessagesWaiting(ListElem->ack_queue);
    for(uint8_t i = 0; i < ack_cnt; i++){                                        // check every sequence number in ack queue
        if( xQueueReceive(ListElem->ack_queue, &ack_seqnum, 0) == pdTRUE ){     // get sequence number from ack queue
            PoolElem_t* PoolElem = xPacketPoolFindElem(ack_seqnum);             // look pool element up from packet pool
            if (PoolElem == NULL) continue;
            if (boPacketInReport(recep_rep, ack_seqnum)){                       // if packet in reception report
                vTagPacketInPeport(PoolElem);                                   // tag element as "in reception report" in packet pool
            }else{
                BaseType_t ret;
                ret = xQueueSend(ListElem->ack_queue, &ack_seqnum, 0);          // place unacknowledged sequence queue back into ack queue
                assert(ret == pdTRUE);
                vTagPacketUntouched(PoolElem);                                  // remove existing tag in packet pool so it wont be garbage collected
            }
        }
    }
}

// garbage collect all packets from packet pool and pushes all packets that have not been acknowledged by all peers back into coding rotation
void vScheduleRetransmissions(){
    uint8_t resend_cnt = xGrgCollectAndRetransmit(getPeerCount()-1, list->retrans_queue);
    if (resend_cnt != 0) ESP_LOGE(TAG_POOL, "Number of retransmissions: %d", resend_cnt);
    else                 ESP_LOGE(TAG_POOL, "No retransmissions scheduled");
}

// refreshes all acknowledgement queues with seqnum of native peer exept that of the source mac address
void vRefreshAckQueues(uint16_t seq_num, const uint8_t mac_addr[ESP_NOW_ETH_ALEN]){
    assert(list != NULL);
    assert(list->pxHead != NULL);

    uint64_t srcItemVal = calcItemValue(mac_addr);

    xPeerElem_t* ListElem = list->pxHead;
    while(ListElem != NULL){
        if (ListElem->item_value != srcItemVal){
            BaseType_t ret_queue = xQueueSend(ListElem->ack_queue, &seq_num, 0);   // add sequence number to acknowledgement queue of peer
            assert(ret_queue == pdTRUE);
        }
        ListElem = ListElem->pxNext;
    }
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
    uint8_t* bloomfilter = bytestream + 1 + sizeof(bloom) - sizeof(bloom.bf);       // pointing to bloom filter in bytestream

    memcpy(&bloom, bloomheader, sizeof(bloom) - sizeof(bloom.bf));                  // copy bloom header into struct
    bloom.bf = (uint8_t*)calloc(bloom.bytes, 1);                                    // allocate memory for bloomfilter
    assert(bloom.bf != NULL);
    memcpy(bloom.bf, bloomfilter, bloom.bytes);                                     // copy bloom filter from bytestream into allocated memory

    uint16_t crc_recv;
    memcpy(&crc_recv, &bytestream[size - sizeof(uint16_t)], sizeof(uint16_t));      // extract crc from bytestream
    uint16_t crc_calc = esp_crc16_le(UINT16_MAX, bytestream, size-sizeof(uint16_t));    // calculate crc16 over bloomfilter

    if (crc_calc != crc_recv){                                                          // compare checksums
        ESP_LOGE(TAG_RECREP, "Parsing reception report failed - invalid crc!");
        ESP_LOGE(TAG_RECREP, "crc_calc = %d", crc_calc);
        ESP_LOGE(TAG_RECREP, "crc_recv = %d", crc_recv);
        assert(1 == 0);
    }

    free(bytestream);   // free memory of byte stream
    return bloom;
}

//////////////////// CODING FUNCTIONS ////////////////////

const char* TAG_CODING = "coding";

// parses the serialized onc data into the struct and frees data
onc_data_t* parse_serialized_onc_data(uint8_t* data){
    onc_data_t* parsed = (onc_data_t*) calloc(1, sizeof(onc_data_t));
    assert(parsed != NULL);
    parsed->type = data[0];
    assert(parsed->type = ONC_DATA);

    parsed->pckt_cnt = data[1];          // extract packet count
    
    parsed->pckt_id_array = (uint16_t*)calloc(sizeof(uint16_t), parsed->pckt_cnt);                           // allocate memory for pacekt id array - needs to be freed later on
    assert(parsed->pckt_id_array != NULL);

    memcpy(parsed->pckt_id_array, &data[2], sizeof(uint16_t)*parsed->pckt_cnt);                             // copy packet ids into id array field
    memcpy(parsed->encoded_data, &data[sizeof(uint16_t)*parsed->pckt_cnt + 2], sizeof(native_data_t));      // copy encoded data to struct

    free(data);
    return parsed;
}

// refreshes virtual queue of source peer with the sequence number of packet
void vRefreshVirtualQueue(uint16_t seq_num, const uint8_t mac_addr[ESP_NOW_ETH_ALEN]){
    assert(list->pxHead != NULL);

    xPeerElem_t* ListElem = xFindElem(mac_addr);                                // find peer in list
    assert(ListElem != NULL);
    BaseType_t ret_queue = xQueueSend(ListElem->virtual_queue, &seq_num, 0);    // add sequence number to virtual queue of peer
    assert(ret_queue == pdTRUE);
}

// returns true if encoding was successfull and writes it into enc_pckt  
// !!! pckt_id_array of onc_data_t enc_pckt will be allocated internally and needs to be freed eventually
bool boGenerateCodedPaket(onc_data_t* enc_pckt){                                               // I had a much better algorithm but since changed the coding scheme it ended out like this
    assert(list->pxHead != NULL);
    assert(list->pxHead->pxNext != NULL);   // need two peers to encode packets

    enc_pckt->type = ONC_DATA;

    uint16_t id_buff[getPeerCount()];           // sequence number buffer for list in coded packet frame
    uint8_t pckt_cnt = 0;
    char str_buff[6*getPeerCount() + 1];        // string buffer for logging
    str_buff[0] = '\0';

    uint16_t seqnum = 0;                // sequence number buffer
    uint8_t wrong_round_count = 0;      // counter of empty virtual queue to distinguish between 

    FIND_PACKET:                             // label to jump back to

    bool prio_avail = (0 != uxQueueMessagesWaiting(list->pxPrio->virtual_queue));   // flag if prioretised peer has packet in virtual queue

    if ( !prio_avail ) return false;                                                // return if prio virtual queue is empty

    // find available packet in virtual queues to code together with virtual queue of prioritised peer
    xPeerElem_t* PeerIt = (list->pxPrio->pxNext == NULL) ? list->pxHead : list->pxPrio->pxNext;     // peer iterator points to neighbor next to coidng pointer - or head if coding pointer points to last peer in list
    while(PeerIt != list->pxPrio){      	                                                        // find available packet of current coding round to code together with the packet pointet to by coding pointer   
        
        if (xQueueReceive(list->retrans_queue, &seqnum, 0) == pdTRUE){                              // prioritise retransmission queue over peer iterator
            native_data_t* pckt = xPacketPoolGetPacket(seqnum);                                     // look up packet with the sequence number
            assert(pckt != NULL);
            memcpy(enc_pckt->encoded_data, pckt, sizeof(native_data_t));                // copy packet into encoded packet (XORing with zero is always just copying)
            id_buff[pckt_cnt] = seqnum;                                                 // add sequence number of native packet to packet id list of onc packet 
            pckt_cnt++;                                                                 // increase seqnum counter (redundant I know since only two packets will be encoded)

            char temp[7];
            sprintf(temp, "%5d ", seqnum);                                              // save string of packet id in string list for later logging
            strcat(str_buff, temp);
            break;                                                                      // break while loop since next packet has been found
        
        }
        
        if (PeerIt->virtHeadRound == list->pxPrio->virtHeadRound){                            // and packet of virtual queue head is in the same coding round  
            
            if (xQueueReceive(PeerIt->virtual_queue, &seqnum, 0) == pdTRUE){                        // can retreive sequence number from virtual queue
                PeerIt->virtHeadRound = !PeerIt->virtHeadRound;                                     // switch round identifier
                native_data_t* pckt = xPacketPoolGetPacket(seqnum);                         // look up packet with the sequence number
                assert(pckt != NULL);
                memcpy(enc_pckt->encoded_data, pckt, sizeof(native_data_t));                // copy packet into encoded packet (XORing with zero is always just copying)
                id_buff[pckt_cnt] = seqnum;                                                 // add sequence number of native packet to packet id list of onc packet 
                pckt_cnt++;                                                                 // increase seqnum counter (redundant I know since only two packets will be encoded)

                char temp[7];
                sprintf(temp, "%5d ", seqnum);                                              // save string of packet id in string list for later logging
                strcat(str_buff, temp);
                break;                                                                      // break while loop since next packet has been found
            }else{
                return false;
            }

        }else{
            wrong_round_count++;
        }
        PeerIt = (PeerIt->pxNext == NULL) ? list->pxHead : PeerIt->pxNext;      // peer iterator points to next peer (wrap around if end of list is reached)
    }
    
    if (PeerIt == list->pxPrio){                                                            // no packet to encode has been found because either  
        if(wrong_round_count == getPeerCount()-1){                                          // coding round is over
            if( xQueueReceive(list->pxPrio->virtual_queue, &seqnum, 0) == pdTRUE){
                list->pxPrio->virtHeadRound = !list->pxPrio->virtHeadRound;                             // switch to label of head for next round
                list->pxPrio = (list->pxPrio->pxNext == NULL) ? list->pxHead : list->pxPrio->pxNext;    // shift priority to next peer
                goto FIND_PACKET;                                                                       // try to find a packet fro the new prio with new coding round again
            }
        }else{                                                                          // packet of current coding round is still missing
            return false;
        }
    }

    // now XOR packet head of prio peer into encoded packet
    
    assert(xQueuePeek(list->pxPrio->virtual_queue, &seqnum, 0) == pdTRUE);    // get sequence number from virtual queue of peer which coding pointer points (do not delete)   
    
    native_data_t* pckt = xPacketPoolGetPacket(seqnum);                         // look up packet with sequence number of vitual queue of coding pointer peer
    assert(pckt != NULL);
    
    uint8_t* bytes = (uint8_t*)pckt;                                            // XOR packet bitwise from virtual queue into onc data field
    for(uint8_t i = 0; i < sizeof(native_data_t); i++){
        enc_pckt->encoded_data[i] ^= bytes[i];
    }
    char temp[7];
    sprintf(temp, "%5d ", seqnum);                                              // save string of packet id in string list for later logging
    strcat(str_buff, temp);

    id_buff[pckt_cnt] = seqnum;                                                 // add sequence number of native packet to packet id list of onc packet 
    pckt_cnt++;                                                                 // increase packet count used for encoding

    // fill encoded packet struct
    enc_pckt->pckt_cnt = pckt_cnt;
    enc_pckt->pckt_id_array = (uint16_t*)calloc(pckt_cnt, sizeof(uint16_t));
    memcpy(enc_pckt->pckt_id_array, id_buff, pckt_cnt*sizeof(uint16_t));

    ESP_LOGW(TAG_CODING, "Encoded packets [ %s]", str_buff);

    return enc_pckt;
}

// returns true if decoding was successfull and writes decoded packet into delPckt
bool boDecodePacket(native_data_t* decPckt, onc_data_t* encPckt){
    assert(encPckt != NULL);

    char str_buff[6*getPeerCount() + 1];
    str_buff[0] = '\0';

    for(uint8_t i = 0; i < encPckt->pckt_cnt; i++){         // read sequence enumbers from packet and as save string
        char temp[7];
        sprintf(temp, "%5d ", encPckt->pckt_id_array[i]);         
        strcat(str_buff, temp);
    }

    memcpy(decPckt, encPckt->encoded_data, sizeof(native_data_t));      // copy encoded data
    uint8_t* decPckt_bytes = (uint8_t*) decPckt;

    uint8_t missing_cnt = 0;
    char str_missing[6*getPeerCount()+1];       // string buffer for missing packets
    str_missing[0] = '\0';

    for (uint8_t i = 0; i < encPckt->pckt_cnt; i++){                                    // find every packet used for encoding in packet pool and XOR it with encoded packet
        native_data_t* pckt = xPacketPoolGetPacket(encPckt->pckt_id_array[i]);          // get packet from packet pool
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
        ESP_LOGE(TAG_CODING, "Cashing encoded packet for later decoding");
        xQueueSend(list->onc_queue, &encPckt, 0);                                        // place pointer to decoded packet into coding cash
        free(decPckt);
        return false;
    }

    if (missing_cnt == 0){                                                               // found every packet in id array - no additional information gained
        ESP_LOGE(TAG_CODING, "Decoding redundant - packets [ %s] already exist in packet pool", str_buff);
        free(decPckt);
        free(encPckt->pckt_id_array);
        free(encPckt);
        return false;
    }

    if (decPckt->type != NATIVE_DATA){                                                  // decode failed if type not correct
        ESP_LOGE(TAG_CODING, "Decoding failed - undefined type %d", decPckt->type);
        free(decPckt);
        free(encPckt->pckt_id_array);
        free(encPckt);
        return false;
    }

    uint16_t crc16_calc = esp_crc16_le(UINT16_MAX, decPckt->payload, PAYLOAD_LEN*sizeof(uint8_t));      // calculate crc16 of decoded packet
    if (decPckt->crc16 != crc16_calc){
        ESP_LOGE(TAG_CODING, "Decoding failed - invalid checksum");
        free(decPckt);
        free(encPckt->pckt_id_array);
        free(encPckt);
        return false;                                                                                   // and compare with crc in crc field
    }

    ESP_LOGW(TAG_CODING, "Decoded packet %d from [ %s]", decPckt->seq_num, str_buff);
    free(encPckt->pckt_id_array);
    free(encPckt);

    return true;
}

uint8_t getCashedPacketCnt(){
    return uxQueueMessagesWaiting(list->onc_queue);
}

// returns true if decoding from cash was successfull and writes decoded packet into delPckt
bool boDecodePacketFromCash(native_data_t* decPckt){

    onc_data_t* encPckt = NULL;
    if ( xQueueReceive(list->onc_queue, &encPckt, 0) != pdTRUE) return false;           // get packet from cash

    char str_buff[6*getPeerCount() + 1];
    str_buff[0] = '\0';

    memcpy(decPckt, encPckt->encoded_data, sizeof(native_data_t));      // copy encoded data
    uint8_t* decPckt_bytes = (uint8_t*) decPckt;

    for(uint8_t i = 0; i < encPckt->pckt_cnt; i++){         // read sequence numbers from packet and as save string
        char temp[7];
        sprintf(temp, "%5d ", encPckt->pckt_id_array[i]);         
        strcat(str_buff, temp);
    }

    uint8_t missing_cnt = 0;
    for (uint8_t i = 0; i < encPckt->pckt_cnt; i++){                                     // find every packet used for encoding in packet pool and XOR it with encoded packet
        native_data_t* pckt = xPacketPoolGetPacket(encPckt->pckt_id_array[i]);           // get packet from packet pool (registers coding tag internally)
        if(pckt != NULL){                                                               // if packet lookup successfull
            uint8_t* temp_bytes = (uint8_t*)pckt;                                       // XOR packet bitwise from virtual queue with encoded onc data field
            for(uint8_t j = 0; j < sizeof(native_data_t); j++){
                decPckt_bytes[j] ^= temp_bytes[j];
            }
        }else{
            missing_cnt++;                                              // increase missing packet count
        }
    }

    if (missing_cnt > 1){                           // decode failed - still more than one packet is missing
        xQueueSend(list->onc_queue, &encPckt, 0);   // place pointer to encoded packet back to queue
        free(decPckt);
        return false;
    }

    if (missing_cnt == 0){                          // found every packet in id array - no additional information gained
        ESP_LOGE(TAG_CODING, "Decoding from cash redundant - packets [ %s] already exist in packet pool", str_buff);
        free(decPckt);
        return false;
    }

    if (decPckt->type != NATIVE_DATA){                                                  // decode failed if type not correct
        ESP_LOGE(TAG_CODING, "Decoding from cash failed - undefined type %d", decPckt->type);
        free(decPckt);
        return false;
    }

    uint16_t crc16_calc = esp_crc16_le(UINT16_MAX, decPckt->payload, PAYLOAD_LEN*sizeof(uint8_t));      // calculate crc16 of decoded packet
    if (decPckt->crc16 != crc16_calc){
        ESP_LOGE(TAG_CODING, "Decoding from cash failed - invalid checksum");
        free(decPckt);
        return false;                                                                                   // and compare with crc in crc field
    }

    ESP_LOGW(TAG_CODING, "Decoded cashed packet %d from [ %s]", decPckt->seq_num, str_buff);
    free(encPckt->pckt_id_array);
    free(encPckt);
    return true;
}