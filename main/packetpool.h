#ifndef _INCLUDES_H_
#define _INCLUDES_H_
#include "includes.h"
#include "./components/bloomfilter/bloom.c"
#endif

// Using an internally chained hash map with coalescend hashing to implement the packet pool. 
// The amount of packets in the pool can be defined with the following macro. 

#define POOL_SIZE_IN_PACKETS           150      // maximum packets to be stored in the packet pool
#define HASH_SEED               0x9747b28c      // seed of murmurhash2      

typedef struct PoolElem{
    native_data_t* pckt;    // a packet in the hashtable
    PoolElem_t* next;       // pointer to other packet in hashtable to evade hashing collisions
    PoolElem_t* prev;       // pointer to prevoius position, with this deletion is possible without traversion
}PoolElem_t;

typedef struct PacketPool{
    uint16_t size;                  // maximum packets the packet pool can hold
    uint16_t entries;               // current number of packets the pool holds
    uint16_t idx_free;              // free slot with largest index in hash table 
    PoolElem_t* hashtable;          // actual hashtable array
} PacketPool_t;

static PacketPool_t packet_pool = {0};          // global variable for packet pool

static const char *TAG_POOL = "packet pool";    // tag for log output

// initializes the packet pool with hashmap
void vInitPacketPool(){
    assert(packet_pool.hashtable == NULL);

    packet_pool.size = POOL_SIZE_IN_PACKETS;

    packet_pool.hashtable = (PoolElem_t*)calloc(packet_pool.size, sizeof(PoolElem_t));  // allocate hashtable array 
    assert(packet_pool.hashtable != NULL);
    packet_pool.idx_free = packet_pool.size - 1;        // index of last element of table
    packet_pool.entries = 0;
}

// finds a packet in packet pool and returns it, returns NULL if not found
PoolElem_t* xPacketPoolFindElem(uint16_t seq_num){
    assert(packet_pool.hashtable != NULL);

    uint32_t hash = murmurhash2(&seq_num, sizeof(seq_num), HASH_SEED);  // compute hash through sequence number and hashseed 
    uint32_t index = hash % packet_pool.size;                           // compute hashtable index 

    PoolElem_t* current = &packet_pool.hashtable[index];                
    while (current->pckt != NULL) {                                     // traverse hash chain
        if (current->pckt->seq_num == seq_num) {
            return current;                                             // packet with sequence number found
        }
        current = current->next;
    }

    return NULL;                                                        // sequence number not found
}

// adds pointer of a natve packet to pool - memory for pckt needs to be persistent!
// returns pointer to pool element which is stored in the virtual queuess
PoolElem_t* xPacketPoolAdd(native_data_t* pckt){
    assert(packet_pool.hashtable != NULL);
    
    if (packet_pool.entries >= POOL_SIZE_IN_PACKETS * 0.9){                        // start garbage collecting if over 90% capacity has been reached 
        ESP_LOGE(TAG_POOL, "packet pool is full - init garbage collection");
        // inint garbage collection here
        ESP_LOGE(TAG_POOL, "garbage collection finished");
    }
    
    uint32_t hash = murmurhash2(&pckt->seq_num, sizeof(pckt->seq_num), HASH_SEED);  // compute hash through sequence number and hashseed 
    uint32_t index = hash % packet_pool.size;                                       // compute hashtable index 

    PoolElem_t* current = &packet_pool.hashtable[index];                            // get first hashed pool element

    if (current->pckt == NULL) {                                                    // free space at hashed index 
        current->pckt = pckt;
    }else{                                                                          // traverse chain to last element chain
        while (current->next != NULL){
            current = current->next;
        }

        current->next = &packet_pool.hashtable[packet_pool.idx_free];               // link free slot to last chain element
        current->next->prev = current;                                              // set of prevoius of next to current element
        
        current = current->next;                                                    // move to new last element of chain
        current->pckt = pckt;
        current->next = NULL;
    }
    
    packet_pool.entries++;                                                          // add an entry into to the packet pool header

    while(packet_pool.hashtable[packet_pool.idx_free].pckt != NULL){                // find new largest index of free table 
        packet_pool.idx_free--;
    }

    ESP_LOGW(TAG_POOL, "Added packet %d to packet pool at %p", pckt->seq_num, current);

    return current;
}

// get a native packet from the packet pool through a serial number - returns NULL if not in packetpool
native_data_t* xPacketPoolGetPacket(uint16_t seq_num){
    assert(packet_pool.hashtable != NULL);

    PoolElem_t* ret = xPacketPoolFindElem(seq_num);
    return ret->pckt;
}

// removes a packet through its reference in the hash table
void vRemoveElemFromPool(PoolElem_t* delElem){
    assert(packet_pool.hashtable != NULL);
    //assert(xPacketPoolFindElem(delElem->pckt->seq_num) == delElem);         // REMOVE THIS AFTER TESTING

    free(delElem->pckt);                                                    // free memory of packet
    delElem->pckt = NULL;                                                   // set pointer to zero
    if (delElem->prev == NULL){

    }

    if ((delElem->prev != NULL)) {               
        if (delElem->next != NULL){                 // entry in the middle of a chain
            delElem->prev->next = delElem->next;
            delElem->next->prev = delElem->prev;
        }else{                                      // last entry of chain
            delElem->prev->next = NULL;
        }  
    }

    if (&packet_pool.hashtable[packet_pool.idx_free] < delElem){                                // update largest free index if necessary 
        packet_pool.idx_free = (delElem - &packet_pool.hashtable[0])/ sizeof(PacketPool_t) ;    // compute index through address of compared element        
        ESP_LOGW(TAG_POOL, "Updated largest free index to %d", packet_pool.idx_free);
    }
}

void xRemovePacketFromPool(uint16_t seq_num){
    assert(packet_pool.hashtable != NULL);

    PoolElem_t* delElem = xPacketPoolFindElem(seq_num);
    if (delElem == NULL) return;

    vRemoveElemFromPool(delElem);
}

bloom_t* xPacketPoolToReceptionReport(){
    assert(packet_pool.hashtable != NULL);

    // TO DO
}

//////// FUNCTIONS ONLY FOR DEBUGING PURPOSES ////////

int16_t xPacketPoolFindPacketIndex(uint16_t seq_num){
    for (uint16_t i = 0; i < packet_pool.size; i++){
        if (packet_pool.hashtable[i].pckt->seq_num == seq_num) return (int16_t)i;
    }
    
    return -1;
}

void vPrintPacketPool(){
    ESP_LOGW(TAG_POOL, " idx | seqnum | nxt ");
    for (uint16_t i = 0; i < packet_pool.size; i++){
        ESP_LOGW(TAG_POOL, " %3d | %6X | %3d ", i, packet_pool.hashtable[i].pckt->seq_num, (packet_pool.hashtable[i].next - &packet_pool.hashtable[0]));
    }
}

