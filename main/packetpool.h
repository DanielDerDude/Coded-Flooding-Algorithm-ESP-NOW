#ifndef _INCLUDES_H_
#define _INCLUDES_H_
#include "includes.h"
#endif

#define MAX_PACKETS_IN_POOL           251      // maximum packets to be stored in the packet pool - NEEDS TO BE PRIME NUMBER for uniformly distribution 
#define HASH_SEED               0x9747b28c     // seed for murmurhash2      

enum {
    UNTOUCHED,      // only for relay node   - encoding pending
    NATIVE,         // only for native peer  - tags self generated packet
    DECODED,        // only for native peer  - packet was decoded successfully 
    INRECEPREP,     // both native and relay - packet was sent/received in reception report
    INRETRANSM,     // for relay node only   - packet is in retransmission
};

typedef struct PoolElem{
    native_data_t* pckt;            // a packet in the hashtable
    uint8_t ack_cnt;                // counter how many times this packet was acknowledged
    struct PoolElem* next;          // pointer to other packet in hashtable to evade hashing collisions
    struct PoolElem* prev;          // pointer to prevoius position, with this deletion is possible without traversion
    uint8_t tag;                    // packet tag either untouched, looked up or sent in reception report
}PoolElem_t;

typedef struct PacketPool{
    uint16_t size;                  // maximum packets the packet pool can hold
    uint16_t entries;               // current number of packets the pool holds
    uint16_t decoded_cnt;           // number of packets that where decoded successfully
    uint16_t idx_free;              // free slot with largest index in hash table 
    PoolElem_t* hashtable;          // actual hashtable array
} PacketPool_t;

static PacketPool_t packet_pool = {0};          // global variable for packet pool

static const char *TAG_POOL = "packet pool";    // tag for log output

// finds a pool element in packet pool and returns it, returns NULL if not found
PoolElem_t* xPacketPoolFindElem(uint16_t seq_num){
    assert(packet_pool.hashtable != NULL);
    uint32_t seq_num32 = (uint32_t) seq_num;                                // cast it  into uint32 - murmurhash needs uint32
    uint32_t hash = murmurhash2(&seq_num32, sizeof(seq_num32), HASH_SEED);  // compute hash through sequence number and hashseed
    hash = murmurhash2(&hash, sizeof(hash), HASH_SEED);                     // hash it again
    uint32_t index = hash % packet_pool.size;                               // compute hashtable index 

    PoolElem_t* current = &packet_pool.hashtable[index];                    
    while (current != NULL) {                                               // traverse bucket chain
        if (current->pckt != NULL){
            if (current->pckt->seq_num == seq_num) return current;          // return current link if sequence number matches
        }
        current = current->next;
    }
    return NULL;                                                            // packet not found
}

// initializes the packet pool with an internally chained hash map with coalescend hashing
void vInitPacketPool(){
    assert(packet_pool.hashtable == NULL);

    packet_pool.size = MAX_PACKETS_IN_POOL;

    packet_pool.hashtable = (PoolElem_t*)calloc(packet_pool.size, sizeof(PoolElem_t));  // allocate hashtable array

    assert(packet_pool.hashtable != NULL);
    
    packet_pool.idx_free = packet_pool.size - 1;        // index of last element of table
    packet_pool.entries = 0;
}

// removes a packet and its reference in the hash table
static void vRemoveElemFromPool(PoolElem_t* delElem){
    assert(packet_pool.hashtable != NULL);
    assert(delElem!= NULL);
    assert(delElem->pckt != NULL);

    free(delElem->pckt);                                                    // free memory of packet
    delElem->pckt = NULL;

    if ( (delElem->next == NULL) && (delElem->prev == NULL) ){              // single link in chain

        memset(delElem, 0, sizeof(PoolElem_t));                             // reset pool element 

        if (&packet_pool.hashtable[packet_pool.idx_free] < delElem){                // deleted element has higher index that current largest index      
            packet_pool.idx_free = (uint16_t)(delElem - packet_pool.hashtable);     // compute new largest index
        }

    }else if ( (delElem->next != NULL) && (delElem->prev == NULL) ){        // first link in chain 

        PoolElem_t* scndLink = delElem->next;                               // save second chain link
        scndLink->prev = NULL;                                              // delete reference to prevoius delElem
        memcpy(delElem, scndLink, sizeof(PoolElem_t));                      // copy second link in first link         
        memset(scndLink, 0, sizeof(PoolElem_t));                            // reset second link 

        if (&packet_pool.hashtable[packet_pool.idx_free] < scndLink){               // if second chain links index is higher than the largest free index
            packet_pool.idx_free = (uint16_t)(scndLink - packet_pool.hashtable);    // compute new largest index
        }

    }else if ( (delElem->next != NULL) && (delElem->prev != NULL) ){        // in the middle of chain

        delElem->prev->next = delElem->next;                                // relink chain
        delElem->next->prev = delElem->prev;
        memset(delElem, 0, sizeof(PoolElem_t));                             // reset pool element 

        if (&packet_pool.hashtable[packet_pool.idx_free] < delElem){                // deleted element has higher index that current largest index      
            packet_pool.idx_free = (uint16_t)(delElem - packet_pool.hashtable);     // compute new largest index
        }

    }else if ( (delElem->next == NULL) && (delElem->prev != NULL) ){        // last in chain

        delElem->prev->next = NULL;                                         // delete reference to last link
        memset(delElem, 0, sizeof(PoolElem_t));                             // reset pool element 

        if (&packet_pool.hashtable[packet_pool.idx_free] < delElem){                // deleted element has higher index that current largest index      
            packet_pool.idx_free = (uint16_t)(delElem - packet_pool.hashtable);     // compute new largest index
        }
    }
    
    packet_pool.entries--;                                                  // reduce entries by one
}

// garbage collects every packet from the packet pool which is tagged as "INRECEPREP"
void vGarbageCollectInRecepRep(){
    ESP_LOGE(TAG_POOL, "Deleting acknowledged packets - packet pool at %d entries", packet_pool.entries);
    uint8_t entries_before = packet_pool.entries;
    for(uint8_t i = 0; i < packet_pool.size; i++){                                      // go through every slot in hashmap
        if(packet_pool.hashtable[i].tag == INRECEPREP){                                 // check if packet was in last reception report
            vRemoveElemFromPool(&packet_pool.hashtable[i]);
        }
    }
    ESP_LOGE(TAG_POOL, "finished - removed %d entries", entries_before- packet_pool.entries);
}

// retransmits packets that have a lower ack count then ack_cnt - else garbage collects them
uint8_t xGrgCollectAndRetransmit(uint8_t ack_cnt, QueueHandle_t retrans_queue){
    ESP_LOGE(TAG_POOL, "Deleting acknowledged packets - packet pool at %d entries", packet_pool.entries);
    uint8_t entries_before = packet_pool.entries;
    uint8_t resend_cnt = 0;
    for(uint8_t i = 0; i < packet_pool.size; i++){                                      // go through every slot in hashmap
        if(packet_pool.hashtable[i].tag == INRECEPREP){                                 // check if packet was in last reception report
            if (packet_pool.hashtable[i].ack_cnt >= ack_cnt){                           // if all peers have acked this packet
                vRemoveElemFromPool(&packet_pool.hashtable[i]);                         // garbage collect packet
            }else{                                                      
                uint16_t seqnum = packet_pool.hashtable[i].pckt->seq_num;                
                BaseType_t ret = xQueueSend(retrans_queue, &seqnum, 0);           // place packet with missing acknowledgements into retransmission queue
                assert(ret = pdTRUE);
                resend_cnt++;
            }
        }
    }
    ESP_LOGE(TAG_POOL, "finished - removed %d entries", entries_before- packet_pool.entries);
    return resend_cnt;
}

// tags a pool element as a self geretated native pacekt
void vTagPacketAsNative(PoolElem_t* Elem){
    assert(Elem != NULL);
    assert(Elem->tag != DECODED);
    Elem->tag = NATIVE;
}

// tags a pool element as decoded and increases the encoded counter
void vTagPacketAsDecoded(PoolElem_t* Elem){
    assert(Elem != NULL);
    if(Elem->tag != DECODED) packet_pool.decoded_cnt++;
    Elem->tag = DECODED;
}

//tag packet as used in reception report
void vTagPacketInPeport(PoolElem_t* Elem){
    assert(Elem != NULL);
    if(Elem->tag == DECODED) packet_pool.decoded_cnt--;       // decrease tag counter if packet was tagged as used in encoding
    Elem->tag = INRECEPREP;
    Elem->ack_cnt++;
}

//tag packet as used in reception report
void vTagPacketUntouched(PoolElem_t* Elem){
    if(Elem->tag == DECODED) packet_pool.decoded_cnt--;       // decrease tag counter if packet was tagged as used in encoding
    Elem->tag = UNTOUCHED;
}

// get number of packets in packet pool tagged as decoded
uint8_t xPacketPoolGetDecodeCnt(){
    return packet_pool.decoded_cnt;
}

// adds pointer of a natve packet to pool - memory for pckt needs to be persistent!
// returns pointer to pool element which is stored in the virtual queuess
PoolElem_t* xPacketPoolAdd(native_data_t* pckt){
    assert(packet_pool.hashtable != NULL);
    assert(pckt != NULL);
    
    if (packet_pool.entries >= MAX_PACKETS_IN_POOL * 0.9){                         // start garbage collecting if over 90% capacity has been reached 
        assert(1 == 0);
    }
    
    uint32_t seq_num32 = (uint32_t) pckt->seq_num;
    uint32_t hash = murmurhash2(&seq_num32, sizeof(seq_num32), HASH_SEED);          // compute hash through sequence number and hashseed
    hash = murmurhash2(&hash, sizeof(hash), HASH_SEED);                             // hash it again
    uint32_t index = hash % packet_pool.size;                                       // compute hashtable index 

    PoolElem_t* current = &packet_pool.hashtable[index];                            // get first hashed pool element

    if (current->pckt == NULL) {                                                    // free space at hashed index 
        current->pckt = pckt;
        current->next = NULL;
        current->prev = NULL;
    }else{                                                                          // traverse chain to last chain element
        while (current->next != NULL){
            current = current->next;
        }

        PoolElem_t* newLink = &packet_pool.hashtable[packet_pool.idx_free];         // new link occupies largest free index
        newLink->pckt = pckt;                                                       // and points to new pckt
        index = packet_pool.idx_free;

        current->next = newLink;                                                    // last element in chain points to new link
        newLink->prev = current;                                                    // set of prevoius of next to current element
        newLink->next = NULL;
    }
    
    packet_pool.entries++;                                                          // increase entry counter

    if (index == packet_pool.idx_free){
        while(packet_pool.hashtable[packet_pool.idx_free].pckt != NULL){            // find new largest index of free table 
            packet_pool.idx_free--;
            assert(packet_pool.idx_free != 0);                                      // this should not be reached
        }
    }

    current->tag = UNTOUCHED;     // set look up flag to false - first time saving
    return current;
}

// get a native packet from the packet pool through a serial number - returns NULL if not in packetpool
native_data_t* xPacketPoolGetPacket(uint16_t seq_num){
    assert(packet_pool.hashtable != NULL);

    PoolElem_t* ret = xPacketPoolFindElem(seq_num);     // find the pool element with the sequence number
    if (ret == NULL) return NULL;

    return ret->pckt;
}

// deletes every packet from packet pool and frees allocated memory 
void vDeletePacketPool(){
    for(uint8_t i = 0; i < packet_pool.size; i++){
        if(packet_pool.hashtable[i].pckt != NULL) free(packet_pool.hashtable[i].pckt);            // free every packet
    }
    free(packet_pool.hashtable);                        // free hashtable array 
    memset(&packet_pool, 0, sizeof(packet_pool));       // zero out the struct on stack
}