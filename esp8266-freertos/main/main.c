#include <string.h>
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>

#include <esp_system.h>
#include <inttypes.h>

#include <esp_log.h>
#include <esp_err.h>
#include <esp_timer.h>
#include <esp_netif.h>
#include <esp_event.h>
#include <esp_sleep.h>
#include <esp_sntp.h>
#include <esp_wifi.h>

#include <nvs.h>
#include <nvs_flash.h>
#include <time.h>
#include "app_config.h"
#include "power_save.h"


#include "mex.h"

#define ADAPT
// #define MQTT

#ifdef MQTT
    #include "mqtt_client.h"
#endif


#define RANDOM
#define POWER_SAVE

#define APP_WIFI_SSID      CONFIG_WIFI_SSID
#define APP_WIFI_PASS      CONFIG_WIFI_PASSWORD
#define APP_MAXIMUM_RETRY  CONFIG_MAXIMUM_RETRY

#define APP_BROKER_HOST    CONFIG_BROKER_HOST
#define APP_BROKER_PORT    CONFIG_BROKER_PORT

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t xEventBits;
struct mex_client mc;

static const char *TAG = "application using mex";

static int s_retry_num = 0;

char cpu_time_used_str[20];

//duty cicle
unsigned int loop_interval = 30;

unsigned int interval_counter = 0;

struct parameters param;

/*-----------WIFI----------------*/
static void event_handler(void* arg, esp_event_base_t event_base,int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < APP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(xEventBits, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:%s", ip4addr_ntoa(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(xEventBits, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta()
{
    xEventBits = xEventGroupCreate();

    tcpip_adapter_init();

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = APP_WIFI_SSID,
            .password = APP_WIFI_PASS
        },
    };

    if (strlen((char *)wifi_config.sta.password)) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");


    EventBits_t bits = xEventGroupWaitBits(xEventBits,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", APP_WIFI_SSID, APP_WIFI_PASS);
                 
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s", APP_WIFI_SSID, APP_WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }

    ESP_ERROR_CHECK(esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler));
    ESP_ERROR_CHECK(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler));
    
    vEventGroupDelete(xEventBits);
}


/*------------------APPLICATION----------------------*/

void mex_task() {

    char topic[] = "water-level";
    const char msg_template[] = "{'distance': %d, 'battery': %d, 'timestamp': '%s'}";
    char msg[sizeof(msg_template) + sizeof(cpu_time_used_str)+ 20];

    int seed = esp_random();
    srand(seed);



    param.battery = 100;
    param.temperature = 30;
    param.distance = rand() % 21;

    param.reservoir_capacity = (1* 10*20) * 10;
    param.fluid_volume = (1* 10*param.distance) * 10;

    sprintf(msg, msg_template, param.distance, param.battery, cpu_time_used_str);

#ifdef MQTT

    esp_mqtt_client_handle_t client;

    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = "mqtt://192.168.0.111",
        .port = 1883,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(client);

    esp_mqtt_client_publish(client, topic, msg, 0, 1, 0);

#else

    mc = create_connection(APP_BROKER_HOST, 60000);

    if (mc.st == CONNECTED) {
        ESP_LOGI(TAG, "Connected to broker");
    } else {
        ESP_LOGI(TAG, "Failed to connect to broker");
        return;
    }

    publish(&mc, topic, msg);
    
    ESP_LOGI(TAG, "Message sent: %s to topic: %s", msg, topic);

#endif
}

void adaptation() {
    ESP_LOGI(TAG, "Adaptation is running");
    struct adapter ad = adapter_init("192.168.0.111", 60010);

    param.battery = 100;
    param.temperature = 30;

    submit_data(&ad, 4,
     "fluid_volume", "800.0",
     "reservoir_capacity", "1000.0", 
     "temperature", "30", 
     "battery", "100");
    
    loop_interval = adapt(ad.sock_fd);
    ESP_LOGI(TAG, "Loop interval: %d", loop_interval);
}


void app_main()
{
    /*---START TIMER---*/
    int64_t start = esp_timer_get_time();
    int64_t end;
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    nvs_handle_t nvs_handle;
    err = nvs_open("storage", NVS_READWRITE, &nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "NVS handle opened\n");

        size_t size = 20;

        /*---READ TIMER FROM NVS---*/
        err = nvs_get_str(nvs_handle, "ctu", cpu_time_used_str, &size);

        switch (err) {
            case ESP_OK:
                ESP_LOGI(TAG, "Restart = %s\n", cpu_time_used_str);
                break;
            case ESP_ERR_NVS_NOT_FOUND:
                ESP_LOGI(TAG, "cpu_time_used is not initialized yet!\n");
                break;
            default :
                ESP_LOGI(TAG, "Error (%s) reading!\n", esp_err_to_name(err));
        } 

        err = nvs_get_u32(nvs_handle, "i_c", &interval_counter);

        switch (err) {
            case ESP_OK:
                ESP_LOGI(TAG, "Restart = %d\n", interval_counter);
                break;
            case ESP_ERR_NVS_NOT_FOUND:
                ESP_LOGI(TAG, "interval_counter is not initialized yet!\n");
                break;
            default :
                ESP_LOGI(TAG, "Error (%s) reading!\n", esp_err_to_name(err));
        }

        err = nvs_get_u8(nvs_handle, "li", &loop_interval);

        switch (err) {
            case ESP_OK:
                ESP_LOGI(TAG, "Restart = %d\n", loop_interval);
                break;
            case ESP_ERR_NVS_NOT_FOUND:
                ESP_LOGI(TAG, "loop_interval is not initialized yet!\n");
                break;
            default :
                ESP_LOGI(TAG, "Error (%s) reading!\n", esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    mex_task();

#ifdef ADAPT
    ESP_LOGI(TAG, "loop_interval: %d", loop_interval);
    ESP_LOGI(TAG, "Interval counter: %d", interval_counter);
    if (interval_counter >= 60) {
        adaptation();  
        interval_counter = 0;
    } else {
        ESP_LOGI(TAG, "Adaptation not needed");
    
    }
#endif // ADAPTATION

    end = esp_timer_get_time();
    
    sprintf(cpu_time_used_str, "%" PRId32 "%" PRId32, (int) ((end - start) >> 32), (int)((end - start) /1000));

    /*---SAVE TIMER TO NVS---*/
    err = nvs_set_str(nvs_handle, "ctu", cpu_time_used_str);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Error (%s) writing!\n", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "sleep_counter updated\n");
    }

    err = nvs_commit(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Error (%s) committing!\n", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Changes committed\n");
    }


    if (interval_counter >= 60) {
        err = nvs_set_u32(nvs_handle, "i_c", loop_interval);
    } else {
        interval_counter += loop_interval;
        err = nvs_set_u32(nvs_handle, "i_c", interval_counter);
    }

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Error (%s) writing!\n", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "sleep_counter updated\n");
    }

    err = nvs_commit(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Error (%s) committing!\n", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Changes committed\n");
    }

    err = nvs_set_u8(nvs_handle, "li", loop_interval);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Error (%s) writing!\n", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "sleep_counter updated\n");
    }

    err = nvs_commit(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Error (%s) committing!\n", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Changes committed\n");
    }

    nvs_close(nvs_handle);

    #ifdef POWER_SAVE
        deep_sleep(loop_interval);
    #endif
} 