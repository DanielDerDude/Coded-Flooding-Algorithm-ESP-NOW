#ifndef _INCLUDES_H_
#define _INCLUDES_H_
#include "includes.h"
#endif

#define MAX_PACKETS_IN_POOL           151      // maximum packets to be stored in the packet pool - NEEDS TO BE PRIME NUMBER for uniformly distribution 
#define HASH_SEED               0x9747b28c      // seed for murmurhash2      

enum {
    UNTOUCHED,
    DECODED,
    INRECEPREP,
};

typedef struct PoolElem{
    native_data_t* pckt;            // a packet in the hashtable
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


// PRIVATE: finds a packet in packet pool and returns it, returns NULL if not found
static PoolElem_t* xPacketPoolFindElem(uint16_t seq_num){
    assert(packet_pool.hashtable != NULL);
    uint32_t seq_num32 = (uint32_t) seq_num;                                // cast it  into uint32 - murmurhash needs uint32
    uint32_t hash = murmurhash2(&seq_num32, sizeof(seq_num32), HASH_SEED);  // compute hash through sequence number and hashseed
    hash = murmurhash2(&hash, sizeof(hash), seq_num32);                     // hash it again
    uint32_t index = hash % packet_pool.size;                               // compute hashtable index 

    PoolElem_t* current = &packet_pool.hashtable[index];                    
    while (current != NULL) {                                               // traverse hash chain
        if (current->pckt != NULL){
            if (current->pckt->seq_num == seq_num) return current;          // return current link if sequence number matches
        }
        current = current->next;
    }
    return NULL;                                                            // packet not found
}

// PRIVATE: removes a packet and its reference in the hash table
static void vRemoveElemFromPool(PoolElem_t* delElem){
    assert(packet_pool.hashtable != NULL);
    assert(delElem!= NULL);
    assert(delElem->pckt != NULL);

    free(delElem->pckt);                                                    // free memory of packet
    delElem->pckt = NULL;                                                   // set pointer to zero
    delElem->tag = UNTOUCHED;                                               // untouched packet
    
    if (delElem->prev == NULL){                                             // delElem is head of chain - will be replaced with second link

        if (delElem->next != NULL){                                             // delElem has a link successor
            if ((uintptr_t)&packet_pool.hashtable[packet_pool.idx_free] < (uintptr_t)delElem->next){                            // if address of second link is higher than current addres of slot with highest free index
                packet_pool.idx_free = ((uintptr_t)delElem->next - (uintptr_t)packet_pool.hashtable)/ sizeof(PacketPool_t) ;    // compute index through address of compared element
            }
            PoolElem_t newHead;
            memcpy(&newHead, delElem->next, sizeof(PoolElem_t));                // save second link of chain
            newHead.prev = NULL;                                                // previous of new head is NULL
            memset(delElem->next, 0, sizeof(PoolElem_t));                       // delete second chain link
            memcpy(delElem, &newHead, sizeof(PoolElem_t));                      // overwrite head of chain with second element
        }else{                                                                  // del element was only link in chain
            if ((uintptr_t)&packet_pool.hashtable[packet_pool.idx_free] < (uintptr_t)delElem){                         // update largest free index if necessary 
                packet_pool.idx_free = ((uintptr_t)delElem - (uintptr_t)packet_pool.hashtable)/ sizeof(PoolElem_t);    // compute index through address of compared element        
            }
            memset(delElem, 0, sizeof(PoolElem_t));                             // set delElem to zero
        }
    }else{
        if (delElem->next != NULL){                                         // delElem is link in the middle of a chain
            delElem->prev->next = delElem->next;
            delElem->next->prev = delElem->prev;
        }else{                                                              // delElem is last link in chain
            delElem->prev->next = NULL;
        }

        if ((uintptr_t)&packet_pool.hashtable[packet_pool.idx_free] < (uintptr_t)delElem){                         // update largest free index if necessary 
            packet_pool.idx_free = ((uintptr_t)delElem - (uintptr_t)packet_pool.hashtable)/ sizeof(PoolElem_t);    // compute index through address of compared element        
        }
    }
    packet_pool.entries--;                                                  // reduce entries by one
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

// garbage collects every packet from the packet pool which is tagged as "INRECEPREP"
void vDeletePacketsLastReceptRep(){
    ESP_LOGE(TAG_POOL, "Deleting acknowledged packets - packet pool at %d entries", packet_pool.entries);
    uint8_t entries_before = packet_pool.entries;
    for(uint8_t i = 0; i < packet_pool.size; i++){                                      // go through every slot in hashmap
        if(packet_pool.hashtable[i].tag == INRECEPREP){                                 // check if packet was in last reception report
            vRemoveElemFromPool(&packet_pool.hashtable[i]);
        }
    }
    ESP_LOGE(TAG_POOL, "finished - removed %d entries", entries_before- packet_pool.entries);
}

// tags a pool element as decoded and increases the encoded counter
void vTagPacketAsDecoded(PoolElem_t* Elem){
    if(Elem->tag != DECODED) packet_pool.decoded_cnt++;
    Elem->tag = DECODED;
}

//tag packet as used in reception report
void vTagPacketInPeport(PoolElem_t* Elem){
    if(Elem->tag == DECODED) packet_pool.decoded_cnt--;       // decrease tag counter if packet was tagged as used in encoding
    Elem->tag = INRECEPREP;
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
    hash = murmurhash2(&hash, sizeof(hash), seq_num32);                             // hash it again
    uint32_t index = hash % packet_pool.size;                                       // compute hashtable index 

    PoolElem_t* current = &packet_pool.hashtable[index];                            // get first hashed pool element

    if (current->pckt == NULL) {                                                    // free space at hashed index 
        current->pckt = pckt;
    }else{                                                                          // traverse chain until found a free packet slot
        while (current->next != NULL){
            current = current->next;
            if (current->pckt->seq_num == pckt->seq_num){                           // check if sequence enumber is already in hash map
                if (memcmp(current->pckt, pckt, sizeof(native_data_t)) == 0){       // check if packet or sequence number is dublicate
                    ESP_LOGE(TAG_POOL, "Found dublicate packet in report!");
                }else{
                    ESP_LOGE(TAG_POOL, "Found dublicate sequence number in report!");
                }
                free(pckt);                                                         // in any case free the new packet and leave the old one
                return current;
            }
        }

        current->next = &packet_pool.hashtable[packet_pool.idx_free];               // link free slot to last chain element
        current->next->prev = current;                                              // set of prevoius of next to current element
        
        current = current->next;                                                    // move to new last element of chain
        current->pckt = pckt;
        current->next = NULL;
        ESP_LOGW(TAG_POOL, "Collision in packet pool occured - re-routing packet to index %d", packet_pool.idx_free);
    }
    
    packet_pool.entries++;                                                          // add an entry into to the packet pool header

    if (packet_pool.hashtable[packet_pool.idx_free].pckt != NULL){
        while(packet_pool.hashtable[packet_pool.idx_free].pckt != NULL){            // find new largest index of free table 
            packet_pool.idx_free--;
            assert(packet_pool.idx_free >= 0);
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