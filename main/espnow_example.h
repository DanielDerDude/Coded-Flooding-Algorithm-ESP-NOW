/* ESPNOW Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#ifndef ESPNOW_H
#define ESPNOW_H

/* ESPNOW can work in both station and softap mode. It is configured in menuconfig. */
#if CONFIG_ESPNOW_WIFI_MODE_STATION
#define ESPNOW_WIFI_MODE WIFI_MODE_STA
#define ESPNOW_WIFI_IF   ESP_IF_WIFI_STA 
#else
#define ESPNOW_WIFI_MODE WIFI_MODE_AP
#define ESPNOW_WIFI_IF   ESP_IF_WIFI_AP
#endif

#define ESPNOW_QUEUE_SIZE           6

#define IS_BROADCAST_ADDR(addr) (memcmp(addr, s_broadcast_mac, ESP_NOW_ETH_ALEN) == 0)

#define PAYLOAD_LEN                 8

typedef enum {
    ESPNOW_SEND_CB,
    ESPNOW_RECV_CB,
} espnow_event_id_t;

typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    esp_now_send_status_t status;
    int64_t send_offset;
} espnow_event_send_cb_t;

typedef struct {
    uint8_t mac_addr[ESP_NOW_ETH_ALEN];
    uint8_t *data;
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

// espnow packet type
enum {
    ESPNOW_DATA_BROADCAST,
    ESPNOW_DATA_UNICAST,
    ESPNOW_DATA_MAX,
};

// data structure to broadcasts timestamps
typedef struct {
    uint8_t type;                         // Broadcast or unicast ESPNOW data.
    uint16_t seq_num;                     // Sequence number of ESPNOW data.
    int64_t timestamp;                    // Timestamp of current systime               
} __attribute__((packed)) espnow_timing_data_t;

// data structure for listing native packet ids which are used for encoding a packet
typedef struct {
    uint16_t* array;
    uint8_t arr_len;
} __attribute__((packed)) packet_id_array_t;

// native data to be encoded
typedef struct{
    uint16_t payload[0];
    uint16_t crc;
} __attribute__((packed)) native_data_t;

// esp now data type with encoded data
typedef struct {
    uint8_t type;                         // Broadcast or unicast ESPNOW data.
    uint16_t seq_num;                     // Sequence number of ESPNOW data.
    packet_id_array_t id_array;           // packet id array
    native_data_t encoded_data;
} __attribute__((packed)) espnow_encoded_data_t;

// esp now sending parameters
typedef struct {
    bool unicast;                         //Send unicast ESPNOW data.
    bool broadcast;                       //Send broadcast ESPNOW data.
    uint16_t count;                       //count of frames to be sent.
    uint16_t delay;                       //Delay between sending two ESPNOW data, unit: ms.
    int len;                              //Length of ESPNOW data to be sent, unit: byte.
    uint8_t *buffer;                      //Buffer pointing to ESPNOW data.
    uint8_t dest_mac[ESP_NOW_ETH_ALEN];   //MAC address of destination device.
} espnow_send_param_t;

enum {
    NEIGHBOR_DETECTION,
    MESSAGE_EXCHANGE,
    SHUTDOWN,
    DEEPSLEEP,
};

#endif
