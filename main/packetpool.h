#ifndef _INCLUDES_H_
#define _INCLUDES_H_
#include "includes.h"
#include "./components/bloomfilter/bloom.c"
#endif

// Using an internally chained hash map to implement the packet pool. The amount of packets in the pool can be defined with the following macro.  

#define POOL_SIZE_IN_PACKETS           150      // maximum packets to be stored in the packet pool
#define HASH_SEED               0x9747b28c      // seed of murmurhash2      

typedef struct PoolElem{
    native_data_t* pckt;     // a packet in the hashtable
    PoolElem_t* next;       // pointer to other packet in hashtable to evade hashing collisions
}PoolElem_t;

typedef struct PacketPool{
    uint16_t size;                  // maximum packets the packet pool can hold
    uint16_t entries;               // current number of packets the packets holds
    PoolElem_t* hashtable;          // actual hashtable array
} PacketPool_t;

static PacketPool_t packet_pool = {0};          // global variable for packet pool

static const char *TAG_POOL = "packet pool";    // tag for log output

// initializes the packet pool with hashmap
void vInitPacketPool(){
    assert(packet_pool.hashtable == NULL);

    packet_pool.hashtable = (PoolElem_t*)calloc(POOL_SIZE_IN_PACKETS, sizeof(PoolElem_t));  // allocate hashtable array 
    assert(packet_pool.hashtable != NULL);

    packet_pool.size = POOL_SIZE_IN_PACKETS;    // set size
}

// finds a packe in packet pool and returns it, returns NULL if not found
PoolElem_t* xFindInPacketPool(uint16_t seq_num){
    assert(packet_pool.hashtable != NULL);

    uint32_t hash = murmurhash2(&seq_num, sizeof(seq_num), HASH_SEED);  // compute hash through sequence number and hashseed 
    uint32_t index = hash % packet_pool.size;                           // compute hashtable index 

    PoolElem_t* current = &packet_pool.hashtable[index];                // traverse hash table
    while (current != NULL) {
        if (current->pckt->seq_num == seq_num) {
            return current;                                             // packet with sequence number found
        }
        current = current->next;
    }

    return NULL;                                                        // sequence number not found
}

// adds pointer of a natve packet to pool - memory for pckt already needs persistent!
void vPacketPoolAdd(native_data_t* pckt){
    assert(packet_pool.hashtable != NULL);
    
    if (packet_pool.entries >= POOL_SIZE_IN_PACKETS){                           // check if pakcet pool has reached maximum capacity
        ESP_LOGE(TAG_POOL, "packet pool is full - init garbage collection");
        // inint garbage collection here
    }
    
    uint32_t hash = murmurhash2(&pckt->seq_num, sizeof(pckt->seq_num), HASH_SEED);  // compute hash through sequence number and hashseed 
    uint32_t index = hash % packet_pool.size;                                       // compute hashtable index 

    PoolElem_t* current = &packet_pool.hashtable[index];                            // get first hashed pool element
    while(current != NULL){                                                         // if not NULL traverse chained hashmap and find free space
        hash = murmurhash2(&pckt->seq_num, sizeof(pckt->seq_num), hash);            // compute hash through sequence number and last hash as hashseed 
        index = hash % packet_pool.size;                                            // compute index
        current->next = &packet_pool.hashtable[index];                              // next points to new hashed array location
        current = &packet_pool.hashtable[index];                                    // current now also points to that location 
    }

    current = (PoolElem_t*)malloc(sizeof(PoolElem_t));                  // create a new pool element at free spot in hashmap
    current->pckt = pckt;                                               // pointer of new pool element now points to given packet location
    current->next = NULL;
    
    packet_pool.entries++;                                              // add an entry into to the packet pool header
}

// get a native packet from the packet pool through a serial number - returns NULL if not in packetpool
native_data_t* xPacketPoolGet(uint16_t seq_num){
    assert(packet_pool.hashtable != NULL);



    
}


void xRemovePacketFromPool(uint16_t seq_num){

}

void 

/* 
bloom_t* vPacketPoolToRecepReport(){
    assert(packet_pool != NULL);


}
 */
