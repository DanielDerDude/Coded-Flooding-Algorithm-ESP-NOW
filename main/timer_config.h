#define TIMER_EVT_QUEUE_LEN    1
#define TIMER_DIVIDER         (16)                              // Hardware timer clock divider
#define TIMER_SCALE           (TIMER_BASE_CLK / TIMER_DIVIDER)  // convert counter value to seconds
#define SLEEP_CYCLE_DUR       50000                             // Sleep cycle duration in us

typedef struct {
    int timer_group;
    int timer_idx;
    int alarm_interval;
    bool auto_reload;
} example_timer_info_t;

typedef struct {
    example_timer_info_t info;
    uint64_t timer_counter_value;
} example_timer_event_t;
