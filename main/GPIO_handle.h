#ifndef _INCLUDES_H_
#define _INCLUDES_H_
#include "includes.h"
#endif

#define GPIO_DEBUG_PIN      18
#define GPIO_DEBUG_PIN_SEL  (1ULL<<GPIO_DEBUG_PIN)

// function for registering GPIO pin for testing timing differences between esp32 devices
void setup_GPIO(){
    gpio_config_t io_conf = {};                     // zero initialise config  structure
    io_conf.intr_type = GPIO_INTR_DISABLE;          // disable interrupt    
    io_conf.mode = GPIO_MODE_OUTPUT;                // set as output mode
    io_conf.pin_bit_mask = GPIO_DEBUG_PIN_SEL;      // bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pull_down_en = 1;                       // enable pull-down mode
    io_conf.pull_up_en = 0;                         // disable pull-up mode
    ESP_ERROR_CHECK( gpio_config(&io_conf) );       // configure GPIO with the given settings
}