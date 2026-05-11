#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "esp_sntp.h"
#include <dht.h>
#include "driver/ledc.h"
#include "driver/gpio.h"

// --- КОНФІГУРАЦІЯ ---
#define WIFI_SSID "Tarat"
#define WIFI_PASS "11111111"
#define MAXIMUM_RETRY 5

#define MQTT_HOST "mqtts://f68143ca98eb49bdae36447f65201d17.s1.eu.hivemq.cloud"
#define MQTT_USER "SomeAdmin"
#define MQTT_PASS "SomeAdmin1"
#define MQTT_PORT 8883

#define TOPIC_OUT "board-out"
#define TOPIC_IN "board-in"

static const gpio_num_t DHT_PIN_BATTERY = GPIO_NUM_4;
static const gpio_num_t DHT_PIN_STREET = GPIO_NUM_5;
static const gpio_num_t FAN_PIN = GPIO_NUM_19;

#define SERVO_PIN GPIO_NUM_18
#define SERVO_LEDC_TIMER LEDC_TIMER_0
#define SERVO_LEDC_MODE LEDC_LOW_SPEED_MODE
#define SERVO_LEDC_CHANNEL LEDC_CHANNEL_0
#define SERVO_LEDC_DUTY_RES LEDC_TIMER_14_BIT
#define SERVO_LEDC_FREQUENCY 50
#define SERVO_MIN_PULSEWIDTH_US 500
#define SERVO_MAX_PULSEWIDTH_US 2500

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "LAB_PROTOTYPE";
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
esp_mqtt_client_handle_t global_mqtt_client = NULL;

static int current_servo_angle = -1;
static int current_fan_state = -1;

// --- ПРОТОТИПИ КЕРУВАННЯ ---
void servo_set_angle(int angle);
void fan_set_state(int state);

void hardware_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = SERVO_LEDC_MODE,
        .timer_num = SERVO_LEDC_TIMER,
        .duty_resolution = SERVO_LEDC_DUTY_RES,
        .freq_hz = SERVO_LEDC_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK};
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode = SERVO_LEDC_MODE,
        .channel = SERVO_LEDC_CHANNEL,
        .timer_sel = SERVO_LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = SERVO_PIN,
        .duty = 0,
        .hpoint = 0};
    ledc_channel_config(&ledc_channel);

    gpio_reset_pin(FAN_PIN);
    gpio_set_direction(FAN_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(FAN_PIN, 0);
    current_fan_state = 0;
}

void servo_set_angle(int angle)
{
    if (angle < 0)
        angle = 0;
    if (angle > 180)
        angle = 180;
    if (angle == current_servo_angle)
        return;
    uint32_t pulse_width = SERVO_MIN_PULSEWIDTH_US + ((SERVO_MAX_PULSEWIDTH_US - SERVO_MIN_PULSEWIDTH_US) * angle) / 180;
    uint32_t duty = (pulse_width * (1 << 14)) / 20000;
    ledc_set_duty(SERVO_LEDC_MODE, SERVO_LEDC_CHANNEL, duty);
    ledc_update_duty(SERVO_LEDC_MODE, SERVO_LEDC_CHANNEL);
    current_servo_angle = angle;
    if (global_mqtt_client)
    {
        char msg[32];
        snprintf(msg, sizeof(msg), "servo - %s", angle > 0 ? "open" : "closed");
        esp_mqtt_client_publish(global_mqtt_client, TOPIC_OUT, msg, 0, 1, 0);
    }
}

void fan_set_state(int state)
{
    if (state == current_fan_state)
        return;
    gpio_set_level(FAN_PIN, state);
    current_fan_state = state;
    if (global_mqtt_client)
    {
        char msg[32];
        snprintf(msg, sizeof(msg), "fan - %s", state ? "on" : "off");
        esp_mqtt_client_publish(global_mqtt_client, TOPIC_OUT, msg, 0, 1, 0);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;

    switch ((esp_mqtt_event_id_t)event_id)
    {
    case MQTT_EVENT_CONNECTED:
        esp_mqtt_client_subscribe(client, TOPIC_IN, 1);
        esp_mqtt_client_publish(client, TOPIC_OUT, "ESP32 Full System Online", 0, 1, 0);
        break;
    case MQTT_EVENT_DATA:
        if (strncmp(event->topic, TOPIC_IN, event->topic_len) == 0)
        {
            char cmd[32];
            int len = event->data_len < 31 ? event->data_len : 31;
            memcpy(cmd, event->data, len);
            cmd[len] = '\0';
            if (strcmp(cmd, "fan-on") == 0)
                fan_set_state(1);
            else if (strcmp(cmd, "fan-off") == 0)
                fan_set_state(0);
            else if (strcmp(cmd, "servo-open") == 0)
                servo_set_angle(90);
            else if (strcmp(cmd, "servo-close") == 0)
                servo_set_angle(0);
        }
        break;
    default:
        break;
    }
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
        esp_wifi_connect();
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_num++;
        }
        else
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void mqtt_app_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_HOST,
        .broker.address.port = MQTT_PORT,
        .credentials.username = MQTT_USER,
        .credentials.authentication.password = MQTT_PASS,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    global_mqtt_client = client;
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL);
    wifi_config_t wifi_config = {.sta = {.ssid = WIFI_SSID, .password = WIFI_PASS, .threshold.authmode = WIFI_AUTH_WPA2_PSK}};
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}

void obtain_time(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    int retry = 0;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < 15)
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
    tzset();
}

void dht_task(void *pvParameters)
{
    float t_batt, h_batt, t_out, h_out;
    char mqtt_msg[128];
    time_t now;
    struct tm timeinfo;
    char time_str[32];

    vTaskDelay(5000 / portTICK_PERIOD_MS);
    obtain_time();
    servo_set_angle(0);
    fan_set_state(0);

    while (1)
    {
        bool b_ok = (dht_read_float_data(DHT_TYPE_DHT11, DHT_PIN_BATTERY, &h_batt, &t_batt) == ESP_OK);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        bool o_ok = (dht_read_float_data(DHT_TYPE_DHT11, DHT_PIN_STREET, &h_out, &t_out) == ESP_OK);

        if (b_ok && o_ok)
        {
            if (t_batt > t_out)
            {
                fan_set_state(1);
                servo_set_angle(90);
            }
            else
            {
                fan_set_state(0);
                servo_set_angle(0);
            }

            // Отримуємо поточний час
            time(&now);
            localtime_r(&now, &timeinfo);
            strftime(time_str, sizeof(time_str), "%H:%M:%S", &timeinfo);

            // Формат: temp 28 26 20:43:15
            snprintf(mqtt_msg, sizeof(mqtt_msg), "temp %.0f %.0f %s", t_batt, t_out, time_str);
            esp_mqtt_client_publish(global_mqtt_client, TOPIC_OUT, mqtt_msg, 0, 1, 0);
        }
        vTaskDelay(58000 / portTICK_PERIOD_MS);
    }
}

void app_main(void)
{
    nvs_flash_init();
    hardware_init();
    wifi_init_sta();
    mqtt_app_start();
    xTaskCreate(&dht_task, "dht_task", 4096, NULL, 5, NULL);
}