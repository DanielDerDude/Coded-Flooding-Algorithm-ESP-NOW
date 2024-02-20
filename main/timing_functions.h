#ifndef _INCLUDES_H_
#define _INCLUDES_H_
#include "includes.h"
#endif

// spinlock for time critical code
portMUX_TYPE spinlock = portMUX_INITIALIZER_UNLOCKED;

static IRAM_ATTR int64_t get_systime_us(void){
    taskENTER_CRITICAL(&spinlock);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    int ret = gettimeofday(&tv, NULL);
    
    assert(ret == 0);
    taskEXIT_CRITICAL(&spinlock);
    return (int64_t)tv.tv_sec * 1000000L + (int64_t)tv.tv_usec;
}

/* static IRAM_ATTR int64_t get_time_us(void){

    return time_on_boot + esp_timer_get_time();
} */

/* static IRAM_ATTR void reset_systime(uint64_t time_us){
    struct timeval tv;
    tv.tv_sec = time_us / 1000000L;
    tv.tv_usec = time_us % 1000000L;
    
    int ret = settimeofday(&tv, NULL);
    
    assert(ret == 0);
} */

/* 
static IRAM_ATTR void correct_systime(int64_t offset_us){ // changes time according to offset instantly
    struct timeval new;
    int ret = gettimeofday(&new, NULL);
    new.tv_sec = new.tv_sec + (offset_us/1000000);  // convert microseconds to seconds and substract from now.sec
    new.tv_usec = new.tv_usec + (offset_us % 1000000); // remainder in microseconds substract from now.usec
    
    ret += settimeofday(&new, NULL);

    assert(ret == 0);
}

static IRAM_ATTR void adjust_systime(int64_t offset_us){ // adjusts time gradually - adjustment does not survive in deepsleep
    struct timeval offset;
    offset.tv_sec = offset_us / 1000000;  // convert microseconds to seconds
    offset.tv_usec = offset_us % 1000000; // remainder in microseconds

    int ret = adjtime(&offset, NULL);

    if (ret == -1){
        ESP_LOGE(TAG2, "adjust time failed!");
    }
} */
