#ifndef _INCLUDES_H_
#define _INCLUDES_H_
#include "includes.h"
#include "./bloom.c"
#endif

#define MAX_ENTRIES             200
#define AMOUNT                  MAX_ENTRIES

static const char* TAG_MAIN = "main";

void app_main(void)
{   
    bloom_t filter;
    int8_t ret = bloom_init2(&filter, MAX_ENTRIES, 0.01);
    assert(ret == 0);

    uint32_t seq_array[AMOUNT] = {0};
    
    // add some numbers to bloom filter
    for(uint32_t i = 0; i < AMOUNT; i++){
        uint32_t sequence = esp_random();        // generiere random sequenz nummer
        seq_array[i] = sequence;                // save sequence

        ret = bloom_add(&filter, &sequence, (uint32_t)sizeof(uint32_t));     // add to bloom filter
        
        // print debug stuff
        switch (ret){
            case(-1): ESP_LOGW(TAG_MAIN, "bloom not initialized"); break;
            case( 0): ESP_LOGW(TAG_MAIN, "added %10lu", sequence); break;
            case( 1): ESP_LOGW(TAG_MAIN, "%10lu already added / collision", sequence); break;
        }
    }

    // queue the same numbers
    uint32_t count_fp = 0;
    uint32_t sequence;
    bool found = false;
    uint32_t j;
    for(uint32_t i = 0; i < AMOUNT; i++){
        
        sequence = esp_random();                                                // generiere random sequenz nummer
        ret = bloom_check(&filter, &sequence, (uint32_t)sizeof(uint32_t));      // abfrage im bloom filter

        // debug stuff
        switch (ret){
            case(-1): ESP_LOGW(TAG_MAIN, "bloom not initialized"); break;
            case( 0): ESP_LOGW(TAG_MAIN, "not present"); break;
            case( 1): 
            {   
                found = false;
                for(j = 0; j < AMOUNT; j++){
                    if (seq_array[j] == sequence){
                        found = true;
                        break;
                    }
                }
                if(found) ESP_LOGW(TAG_MAIN, "present");
                else{
                    count_fp++;
                    ESP_LOGW(TAG_MAIN, "false positive");
                }      
                break;
            }
        }
    }


    ESP_LOGE(TAG_BLOOM, "Measured false positive rate: %lf", (double)count_fp/AMOUNT);

    bloom_print(&filter);

    ESP_LOGW(TAG_MAIN, "finished");
}