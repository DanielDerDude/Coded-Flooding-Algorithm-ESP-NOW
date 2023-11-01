#include <sys/time.h>

// function that returns the current time in ms
/* static IRAM_ATTR int64_t get_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)(tv.tv_sec * 1000LL + (int64_t)(tv.tv_usec / 1000LL));
} */

static const char *TAG3 = "timing_functions";

static IRAM_ATTR int64_t get_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000L + (int64_t)tv.tv_usec;
}

static IRAM_ATTR void reset_time(void){
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    int ret = settimeofday(&tv, NULL);

    if (ret == -1){
        ESP_LOGE(TAG3, "Reset time failed!");
    }
}

static IRAM_ATTR void correct_time(int64_t offset_us){ // changes time according to offset instantly
    struct timeval new;
    gettimeofday(&new, NULL);
    new.tv_sec = new.tv_sec - (offset_us/1000000);  // convert microseconds to seconds and substract from now.sec
    new.tv_usec = new.tv_usec - (offset_us % 1000000); // remainder in microseconds substract from now.usec

    int ret = settimeofday(&new, NULL);

    if (ret == -1){
        ESP_LOGE(TAG3, "correct time failed!");
    }
}

/* static IRAM_ATTR void adjust_time(int64_t offset_us){ // adjusts time gradually - adjustment does not survive in deepsleep
    struct timeval offset;
    offset.tv_sec = offset_us / 1000000;  // convert microseconds to seconds
    offset.tv_usec = offset_us % 1000000; // remainder in microseconds

    int ret = adjtime(&offset, NULL);

    if (ret == -1){
        ESP_LOGE(TAG3, "adjust time failed!");
    }
} */
