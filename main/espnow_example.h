#ifndef ESPNOW_H
#define ESPNOW_H

/* ESPNOW can work in both station and softap mode. It is configured in menuconfig. */
#if CONFIG_ESPNOW_WIFI_MODE_STATION
#define ESPNOW_WIFI_MODE WIFI_MODE_STA
#define ESPNOW_WIFI_IF   ESP_IF_WIFI_STA 
#define ESP_MAC_TYPE     ESP_MAC_WIFI_STA
#else
#define ESPNOW_WIFI_MODE WIFI_MODE_AP
#define ESPNOW_WIFI_IF   ESP_IF_WIFI_AP
#define ESP_MAC_TYPE     ESP_MAC_WIFI_SOFTAP
#endif

#define ESPNOW_QUEUE_SIZE           20

#define PAYLOAD_LEN                  4      // neeeds to be a multiple of 4


// controller state is resembled in event bit combination
enum {
    HOLD               = 0,
    INIT_DETECTION     = (1 << 0),
    NEIGHBOR_DETECTION = (1 << 0) | (1 << 1),
    INIT_MSG_EXCHANGE  = (1 << 0) | (1 << 1) | (1 << 2),
    MESSAGE_EXCHANGE   = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3),
    SHUTDOWN           = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4),
    NETWORK_RESET      = (1 << 0) | (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5),
};

// espnow packet types
enum {
    TIMESTAMP,
    ONC_DATA,
    NATIVE_DATA,
    RECEP_REPORT,
    RESET,
    UNDEF,
};

// esp now event types
typedef enum {
    RECV_TIMESTAMP,
    SEND_TIMESTAMP,
    RECV_NATIVE,
    SEND_NATIVE,
    RECV_ONC_DATA,
    SEND_ONC_DATA,
    RECV_RECEPREP,
    SEND_RECEPREP,
    SEND_RESET,
    RECV_RESET,
    EVENT_UNDEF,
} espnow_event_id_t;

typedef struct {
    uint8_t packet_type;            // the packet type of the commsssioned packet
    int64_t time_placed;            // the time the packet was intially commissioned
} last_commission_t;                

typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    esp_now_send_status_t status;
    int64_t send_offset;
} espnow_event_send_cb_t;

typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    uint8_t *data;
    int64_t recv_offset;
    int data_len;
    int sig_len;
} espnow_event_recv_cb_t;

typedef union {
    espnow_event_send_cb_t send_cb;
    espnow_event_recv_cb_t recv_cb;
} espnow_event_info_t;

typedef struct {
    espnow_event_id_t id;
    espnow_event_info_t info;
    int64_t timestamp;
} espnow_event_t;

// data structure to broadcasts timestamps
typedef struct {
    uint8_t type;                           // type for identification and error handeling
    uint16_t seq_num;                       // sequence number of timing data
    int64_t timestamp;                      // Timestamp of current systime               
} __attribute__((packed)) timing_data_t;

// native data with reception report
typedef struct{
    uint8_t type;
    uint16_t seq_num;                       // random sequence number of native data
    uint8_t payload[PAYLOAD_LEN];           // actual native data
    uint16_t crc16;                         // crc value of native data
} __attribute__((packed)) native_data_t;

// struct holding the encoded data and coding overhead
typedef struct {
    uint8_t type;                                   // type for identification and error handeling
    uint8_t pckt_cnt;                               // number of packets used for encoding
    uint16_t* pckt_id_array;                        // array with packet ids used for encoding - variable length given by packt count 
    uint8_t encoded_data[sizeof(native_data_t)];    // pointer to encoded data
} __attribute__((packed)) onc_data_t;

#endif
