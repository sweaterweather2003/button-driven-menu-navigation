#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "driver/gpio.h"
#include "driver/ledc.h"

#define DEBOUNCE_TICKS              4      
#define SHORT_PRESS_MAX_MS          300
#define LONG_PRESS_MIN_MS           1500
#define MULTI_CLICK_TIMEOUT_MS      700
#define AUTO_CYCLE_PERIOD_MS        2000
#define NUM_PATTERNS                4
#define MENU_ITEMS_COUNT            4
#define BRIGHTNESS_MAX              10
#define BRIGHTNESS_MIN              0

static const uint16_t patterns[NUM_PATTERNS] = {
    0xFFFF,     
    0xAAAA,     
    0x5555,     
    0x0F0F      
};

const gpio_num_t BTN1_PIN   = GPIO_NUM_21;   
const gpio_num_t BTN2_PIN   = GPIO_NUM_48;   
const gpio_num_t BTN3_PIN   = GPIO_NUM_47;   
const gpio_num_t DS_PIN     = GPIO_NUM_4;
const gpio_num_t OE_PIN     = GPIO_NUM_5;
const gpio_num_t LATCH_PIN  = GPIO_NUM_6;
const gpio_num_t CLOCK_PIN  = GPIO_NUM_7;
const gpio_num_t MR_PIN     = GPIO_NUM_15;

typedef enum {
    PRESS_SINGLE = 0,
    PRESS_DOUBLE,
    PRESS_TRIPLE,
    PRESS_LONG
} press_type_t;

typedef struct {
    uint8_t      button_id;     
    press_type_t type;
    TickType_t   timestamp;
} button_event_t;

typedef struct {
    uint16_t pattern;
    uint8_t  brightness;        
} display_info_t;

typedef enum {
    STATE_MAIN_MENU = 0,
    STATE_BRIGHTNESS,
    STATE_MODE_SELECT,
    STATE_MANUAL_MODE,
    STATE_AUTO_MODE,
    STATE_INFO,
    STATE_RESET,
    STATE_POWER_OFF
} menu_state_t;

static QueueHandle_t button_event_queue = NULL;
static QueueHandle_t display_queue = NULL;
static SemaphoreHandle_t shift_mutex = NULL;

typedef struct {
    gpio_num_t pin;
    bool       last_state;
    int        debounce_counter;
    TickType_t press_start_time;
    TickType_t last_release_time;
    int        click_counter;
    TickType_t press_times[4];  
} button_tracker_t;

static void shift_out_pattern(uint16_t pattern);
static void ButtonInput_Thread(void *pvParameters);
static void MenuLogic_Thread(void *pvParameters);
static void DisplayManager_Thread(void *pvParameters);

static void shift_out_pattern(uint16_t pattern) {
    
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(DS_PIN, (pattern & (1U << i)) ? 1 : 0);
        gpio_set_level(CLOCK_PIN, 1);
        gpio_set_level(CLOCK_PIN, 0);
    }
    
    for (int i = 15; i >= 8; i--) {
        gpio_set_level(DS_PIN, (pattern & (1U << i)) ? 1 : 0);
        gpio_set_level(CLOCK_PIN, 1);
        gpio_set_level(CLOCK_PIN, 0);
    }
    
    gpio_set_level(LATCH_PIN, 1);
    gpio_set_level(LATCH_PIN, 0);
}

static void ButtonInput_Thread(void *pvParameters) {
    button_tracker_t buttons[3] = {
        {BTN1_PIN, false, 0, 0, 0, 0, {0,0,0,0}},
        {BTN2_PIN, false, 0, 0, 0, 0, {0,0,0,0}},
        {BTN3_PIN, false, 0, 0, 0, 0, {0,0,0,0}}
    };

    while (true) {
        TickType_t now = xTaskGetTickCount();
        for (int i = 0; i < 3; i++) {
            button_tracker_t *b = &buttons[i];
            bool current = (gpio_get_level(b->pin) == 0);

            if (current == b->last_state) {
                b->debounce_counter = 0;
            } else {
                b->debounce_counter++;
                if (b->debounce_counter >= DEBOUNCE_TICKS) {
                    b->debounce_counter = 0;
                    b->last_state = current;

                    if (current) {  
                        Serial.printf("DEBUG: Button %d pressed at %u\n", i+1, now);  
                        b->press_start_time = now;
                        if (b->last_release_time > 0 &&
                            (now - b->last_release_time) < pdMS_TO_TICKS(MULTI_CLICK_TIMEOUT_MS)) {
                            b->click_counter++;
                        } else {
                            b->click_counter = 1;
                        }
                        b->press_times[b->click_counter] = now;
                        b->last_release_time = 0;
                    } else {  
                        Serial.printf("DEBUG: Button %d released at %u\n", i+1, now);  
                        TickType_t duration = now - b->press_start_time;
                        if (duration >= pdMS_TO_TICKS(LONG_PRESS_MIN_MS)) {
                            button_event_t ev = {(uint8_t)(i+1), PRESS_LONG, now};
                            xQueueSend(button_event_queue, &ev, 0);
                            Serial.printf("DEBUG: Queued LONG for Button %d\n", i+1);  
                            b->click_counter = 0;
                            b->last_release_time = 0;
                        } else if (duration <= pdMS_TO_TICKS(SHORT_PRESS_MAX_MS)) {
                            b->last_release_time = now;
                        } else {
                            b->click_counter = 0;
                            b->last_release_time = 0;
                        }
                    }
                }
            }

            
            if (b->click_counter >= 1 && b->last_release_time > 0 &&
                (now - b->last_release_time) >= pdMS_TO_TICKS(MULTI_CLICK_TIMEOUT_MS)) {
                TickType_t current_time = now;
                int count = b->click_counter;
                button_event_t ev = {(uint8_t)(i + 1), PRESS_SINGLE, current_time};

                if (count == 1) {
                    ev.type = PRESS_SINGLE;
                    xQueueSend(button_event_queue, &ev, 0);
                    Serial.printf("DEBUG: Queued SINGLE for Button %d\n", i+1);  
                } else if (count == 2) {
                    TickType_t span = b->press_times[2] - b->press_times[1];
                    if (span < pdMS_TO_TICKS(500)) {
                        ev.type = PRESS_DOUBLE;
                        xQueueSend(button_event_queue, &ev, 0);
                        Serial.printf("DEBUG: Queued DOUBLE for Button %d\n", i+1);  
                    } else {
                        ev.type = PRESS_SINGLE;
                        xQueueSend(button_event_queue, &ev, 0);
                        xQueueSend(button_event_queue, &ev, 0);
                        Serial.printf("DEBUG: Queued two SINGLES for Button %d\n", i+1); 
                    }
                } else if (count == 3) {
                    TickType_t span = b->press_times[3] - b->press_times[1];
                    if (span < pdMS_TO_TICKS(700)) {
                        ev.type = PRESS_TRIPLE;
                        xQueueSend(button_event_queue, &ev, 0);
                        Serial.printf("DEBUG: Queued TRIPLE for Button %d\n", i+1);  
                    } else {
                        TickType_t span_first_two = b->press_times[2] - b->press_times[1];
                        TickType_t span_last_two = b->press_times[3] - b->press_times[2];
                        if (span_first_two < pdMS_TO_TICKS(500)) {
                            ev.type = PRESS_DOUBLE;
                            xQueueSend(button_event_queue, &ev, 0);
                            ev.type = PRESS_SINGLE;
                            xQueueSend(button_event_queue, &ev, 0);
                            Serial.printf("DEBUG: Queued DOUBLE + SINGLE for Button %d\n", i+1);  
                        } else if (span_last_two < pdMS_TO_TICKS(500)) {
                            ev.type = PRESS_SINGLE;
                            xQueueSend(button_event_queue, &ev, 0);
                            ev.type = PRESS_DOUBLE;
                            xQueueSend(button_event_queue, &ev, 0);
                            Serial.printf("DEBUG: Queued SINGLE + DOUBLE for Button %d\n", i+1);  
                        } else {
                            ev.type = PRESS_SINGLE;
                            xQueueSend(button_event_queue, &ev, 0);
                            xQueueSend(button_event_queue, &ev, 0);
                            xQueueSend(button_event_queue, &ev, 0);
                            Serial.printf("DEBUG: Queued three SINGLES for Button %d\n", i+1);  
                        }
                    }
                }
                b->click_counter = 0;
                b->last_release_time = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void MenuLogic_Thread(void *pvParameters) {
    menu_state_t state = STATE_MAIN_MENU;
    int selected_item = 0;
    uint8_t brightness = 5;
    bool is_auto_mode = false;
    int current_pattern_idx = 0;
    int info_page = 0;

    
    bool temp_is_auto_mode = false;
    int  temp_pattern_idx = 0;

    uint16_t initial_pattern = (1U << selected_item);
    display_info_t initial_info = {initial_pattern, brightness};
    xQueueSend(display_queue, &initial_info, portMAX_DELAY);

    while (true) {
        button_event_t ev;
        BaseType_t res;

        if (state == STATE_AUTO_MODE) {
            res = xQueueReceive(button_event_queue, &ev, pdMS_TO_TICKS(AUTO_CYCLE_PERIOD_MS));
            if (res == pdFALSE) {
                current_pattern_idx = (current_pattern_idx + 1) % NUM_PATTERNS;
                display_info_t info = {patterns[current_pattern_idx], brightness};
                xQueueSend(display_queue, &info, 0);
                continue;
            }
        } else {
            res = xQueueReceive(button_event_queue, &ev, portMAX_DELAY);
        }

        Serial.printf("DEBUG: Received event - Button %d, Type %d, State %d, sel=%d\n", 
                      ev.button_id, ev.type, state, selected_item);

        bool update_display = false;

        if (state == STATE_POWER_OFF) {
            if (ev.button_id == 1 && ev.type == PRESS_LONG) {
                state = STATE_MAIN_MENU;
                selected_item = 0;
                brightness = 5;           
                is_auto_mode = false;
                current_pattern_idx = 0;
                update_display = true;
            }
            continue;
        }

        switch (state) {
            case STATE_MAIN_MENU:
                if (ev.button_id == 1 && ev.type == PRESS_SINGLE) {
                    selected_item = (selected_item + 1) % MENU_ITEMS_COUNT;
                    update_display = true;
                } else if (ev.button_id == 3 && ev.type == PRESS_SINGLE) {
                    selected_item = (selected_item - 1 + MENU_ITEMS_COUNT) % MENU_ITEMS_COUNT;
                    update_display = true;
                } else if (ev.button_id == 2 && ev.type == PRESS_SINGLE) {
                    update_display = true;
                    switch (selected_item) {
                        case 0: state = STATE_BRIGHTNESS; break;
                        case 1:
                            state = STATE_MODE_SELECT;
                            temp_is_auto_mode = is_auto_mode;
                            break;
                        case 2: state = STATE_INFO; info_page = 0; break;
                        case 3: state = STATE_RESET; break;
                    }
                } else if (ev.button_id == 3 && ev.type == PRESS_LONG) {
                    state = STATE_POWER_OFF;
                    update_display = true;
                }
                break;

            case STATE_BRIGHTNESS:
                if (ev.button_id == 2 && ev.type == PRESS_SINGLE) {
                    if (brightness < BRIGHTNESS_MAX) brightness++;
                    update_display = true;
                } else if (ev.button_id == 3 && ev.type == PRESS_SINGLE) {
                    if (brightness > BRIGHTNESS_MIN) brightness--;
                    update_display = true;
                } else if (ev.button_id == 1 && ev.type == PRESS_SINGLE) {
                    state = STATE_MAIN_MENU;
                    update_display = true;
                }
                break;

            case STATE_MODE_SELECT:
                if (ev.button_id == 1 && ev.type == PRESS_SINGLE) {
                    temp_is_auto_mode = !temp_is_auto_mode;
                    update_display = true;
                } else if (ev.button_id == 2 && ev.type == PRESS_SINGLE) {
                    is_auto_mode = temp_is_auto_mode;
                    if (is_auto_mode) {
                        state = STATE_AUTO_MODE;
                    } else {
                        state = STATE_MANUAL_MODE;
                        temp_pattern_idx = current_pattern_idx;
                    }
                    update_display = true;
                } else if (ev.button_id == 3 && ev.type == PRESS_SINGLE) {
                    state = STATE_MAIN_MENU;
                    update_display = true;
                }
                break;

            case STATE_MANUAL_MODE:
                if (ev.button_id == 1 && ev.type == PRESS_SINGLE) {
                    temp_pattern_idx = (temp_pattern_idx + 1) % NUM_PATTERNS;
                    update_display = true;
                } else if (ev.button_id == 2 && ev.type == PRESS_SINGLE) {
                    current_pattern_idx = temp_pattern_idx;
                    state = STATE_MAIN_MENU;
                    update_display = true;
                } else if (ev.button_id == 3 && ev.type == PRESS_SINGLE) {
                    state = STATE_MAIN_MENU;
                    update_display = true;
                }
                break;

            case STATE_AUTO_MODE:
                if (ev.button_id == 1 && ev.type == PRESS_SINGLE) {
                    state = STATE_MAIN_MENU;
                    update_display = true;
                }
                break;

            case STATE_INFO:
                if (ev.button_id == 1 && ev.type == PRESS_SINGLE) {
                    info_page = (info_page + 1) % 2;
                    update_display = true;
                } else if (ev.button_id == 3 && ev.type == PRESS_SINGLE) {
                    state = STATE_MAIN_MENU;
                    update_display = true;
                }
                break;

            case STATE_RESET:
                if (ev.button_id == 2 && ev.type == PRESS_DOUBLE) {
                    brightness = 5;
                    is_auto_mode = false;
                    current_pattern_idx = 0;
                    state = STATE_MAIN_MENU;
                    update_display = true;
                } else if (ev.button_id == 3 && ev.type == PRESS_SINGLE) {
                    state = STATE_MAIN_MENU;
                    update_display = true;
                }
                break;

            default:
                break;
        }

        if (!update_display) continue;

        Serial.printf("DEBUG: Updating display - State %d, sel=%d, bright=%d\n", state, selected_item, brightness);

        uint16_t display_pattern = 0;
        switch (state) {
            case STATE_MAIN_MENU:
                display_pattern = (1U << selected_item);
                break;
            case STATE_BRIGHTNESS:
                display_pattern = (brightness == 0) ? 0 : ((1U << brightness) - 1);
                break;
            case STATE_MODE_SELECT:
                display_pattern = temp_is_auto_mode ? 0x000C : 0x0003;  
                break;
            case STATE_MANUAL_MODE:
                display_pattern = patterns[temp_pattern_idx];
                break;
            case STATE_AUTO_MODE:
                display_pattern = patterns[current_pattern_idx];
                break;
            case STATE_INFO:
                display_pattern = (info_page == 0) ? 0x000F : 0x00F0;
                break;
            case STATE_RESET:
                display_pattern = 0xAAAA;
                break;
            case STATE_POWER_OFF:
                display_pattern = 0x0000;
                break;
            default:
                display_pattern = 0x0000;
                break;
        }

        display_info_t info = {display_pattern, brightness};
        xQueueSend(display_queue, &info, 0);
    }
}

static void DisplayManager_Thread(void *pvParameters) {
    int current_brightness = -1;

    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num       = LEDC_TIMER_0,
        .freq_hz         = 1000,
        .clk_cfg         = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer_cfg);

    ledc_channel_config_t ch_cfg = {
        .gpio_num      = OE_PIN,
        .speed_mode    = LEDC_LOW_SPEED_MODE,
        .channel       = LEDC_CHANNEL_0,
        .intr_type     = LEDC_INTR_DISABLE,
        .timer_sel     = LEDC_TIMER_0,
        .duty          = 0,
        .hpoint        = 0,
        .flags         = { .output_invert = 0 }
    };
    ledc_channel_config(&ch_cfg);

    while (true) {
        display_info_t info;
        if (xQueueReceive(display_queue, &info, portMAX_DELAY) == pdTRUE) {
            xSemaphoreTake(shift_mutex, portMAX_DELAY);
            shift_out_pattern(info.pattern);

            if (info.brightness != current_brightness) {
                current_brightness = info.brightness;
                uint32_t max_duty = (1 << 10) - 1;
                uint32_t duty = max_duty * (10 - info.brightness) / 10;  
                ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
                ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
            }
            xSemaphoreGive(shift_mutex);
        }
    }
}

void setup() {
    Serial.begin(115200);  

    gpio_reset_pin(DS_PIN);    gpio_set_direction(DS_PIN, GPIO_MODE_OUTPUT);
    gpio_reset_pin(CLOCK_PIN); gpio_set_direction(CLOCK_PIN, GPIO_MODE_OUTPUT);
    gpio_reset_pin(LATCH_PIN); gpio_set_direction(LATCH_PIN, GPIO_MODE_OUTPUT);
    gpio_reset_pin(OE_PIN);    gpio_set_direction(OE_PIN, GPIO_MODE_OUTPUT);
    gpio_reset_pin(MR_PIN);    gpio_set_direction(MR_PIN, GPIO_MODE_OUTPUT);

    gpio_set_level(MR_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(MR_PIN, 1);

    gpio_reset_pin(BTN1_PIN); gpio_set_direction(BTN1_PIN, GPIO_MODE_INPUT); gpio_pullup_en(BTN1_PIN);
    gpio_reset_pin(BTN2_PIN); gpio_set_direction(BTN2_PIN, GPIO_MODE_INPUT); gpio_pullup_en(BTN2_PIN);
    gpio_reset_pin(BTN3_PIN); gpio_set_direction(BTN3_PIN, GPIO_MODE_INPUT); gpio_pullup_en(BTN3_PIN);

    button_event_queue = xQueueCreate(10, sizeof(button_event_t));
    display_queue      = xQueueCreate(5, sizeof(display_info_t));
    shift_mutex        = xSemaphoreCreateMutex();

    xTaskCreate(ButtonInput_Thread,   "ButtonInput",  4096, NULL, 5, NULL);
    xTaskCreate(MenuLogic_Thread,     "MenuLogic",    4096, NULL, 4, NULL);
    xTaskCreate(DisplayManager_Thread,"DisplayMgr",   4096, NULL, 3, NULL);

    shift_out_pattern(0x0000);
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
