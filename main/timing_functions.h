#include <sys/time.h>

static const char *TAG2 = "systime";

static IRAM_ATTR int64_t get_systime_us(void){
    struct timeval tv;
    int ret = gettimeofday(&tv, NULL);
    
    assert(ret == 0);

    return (int64_t)tv.tv_sec * 1000000L + (int64_t)tv.tv_usec;
}

static IRAM_ATTR void reset_systime(void){
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    
    int ret = settimeofday(&tv, NULL);
    
    assert(ret == 0);
}

static IRAM_ATTR void correct_systime(int64_t offset_us){ // changes time according to offset instantly
    struct timeval new;
    int ret = gettimeofday(&new, NULL);
    new.tv_sec = new.tv_sec + (offset_us/1000000);  // convert microseconds to seconds and substract from now.sec
    new.tv_usec = new.tv_usec + (offset_us % 1000000); // remainder in microseconds substract from now.usec
    
    ret += settimeofday(&new, NULL);

    assert(ret == 0);
}

/* static IRAM_ATTR void adjust_systime(int64_t offset_us){ // adjusts time gradually - adjustment does not survive in deepsleep
    struct timeval offset;
    offset.tv_sec = offset_us / 1000000;  // convert microseconds to seconds
    offset.tv_usec = offset_us % 1000000; // remainder in microseconds

    int ret = adjtime(&offset, NULL);

    if (ret == -1){
        ESP_LOGE(TAG2, "adjust time failed!");
    }
} */
