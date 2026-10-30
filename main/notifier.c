#include "notifier.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_wifi_types_generic.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "nvs_flash.h"
#include "secrets.h"
#include <string.h>

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAILED_BIT     BIT1
#define WIFI_TIMEOUT_MS     20000
#define WIFI_MAX_RETRIES    4

static unsigned int wifi_retry_count;

static const char *TAG = "notifier";

static EventGroupHandle_t wifi_events;
static esp_netif_t *wifi_netif;

static esp_event_handler_instance_t disconnected_handler;
static esp_event_handler_instance_t got_ip_handler;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data){
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP){
        xEventGroupClearBits(wifi_events, WIFI_FAILED_BIT);
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED){
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);

        if (wifi_retry_count < WIFI_MAX_RETRIES){
            wifi_retry_count++;

            ESP_LOGI(TAG, "Retrying Wi-Fi: %u/%u", wifi_retry_count, WIFI_MAX_RETRIES);

            esp_err_t err = esp_wifi_connect();

            if (err == ESP_OK){
                return;
            }

            ESP_LOGW(TAG, "Cannot start connection: %s", esp_err_to_name(err));
        }

        xEventGroupSetBits(wifi_events, WIFI_FAILED_BIT);
    }
}

static void wifi_disconnect(){
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, disconnected_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_handler));

    ESP_ERROR_CHECK(esp_wifi_stop());
    ESP_ERROR_CHECK(esp_wifi_deinit());

    esp_netif_destroy_default_wifi(wifi_netif);
    wifi_netif = NULL;

    vEventGroupDelete(wifi_events);
    wifi_events = NULL;

    ESP_LOGI(TAG, "Wi-Fi disconnected");
}

static esp_err_t wifi_connect(){
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(wifi_events != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    wifi_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(wifi_netif != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT,
        WIFI_EVENT_STA_DISCONNECTED,
        wifi_event_handler,
        NULL,
        &disconnected_handler));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        wifi_event_handler,
        NULL,
        &got_ip_handler));

    wifi_config_t config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting Wi-Fi...");

    wifi_retry_count = 0;

    esp_err_t err = esp_wifi_connect();

    if (err != ESP_OK){
        wifi_disconnect();
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(
        wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(WIFI_TIMEOUT_MS));

    if ((bits & WIFI_CONNECTED_BIT) && !(bits & WIFI_FAILED_BIT)){
        ESP_LOGI(TAG, "Connected to Wi-Fi");
        return ESP_OK;
    }

    err = (bits & WIFI_FAILED_BIT) ? ESP_FAIL : ESP_ERR_TIMEOUT;

    ESP_LOGW(TAG, "Connection error: %s", esp_err_to_name(err));

    wifi_disconnect();
    return err;
}

esp_err_t notifier_send(){
    esp_err_t wifi_err = wifi_connect();

    if (wifi_err != ESP_OK){
        return wifi_err;
    }

    esp_http_client_config_t http_config = {
        .url = NTFY_URL,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);

    const char *post_data = "Pranie ukończone!";
    esp_http_client_set_header(client, "Title", "Pralka");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t post_err;

    for (int i=0; i<3; i++) {
        post_err = esp_http_client_perform(client);
        if(post_err == ESP_OK){
            int status = esp_http_client_get_status_code(client);
            ESP_LOGI(TAG, "HTTP POST status = %d", status);

            if (status >= 200 && status < 300){
                break;
            }

            post_err = ESP_FAIL;

            bool retry = (status == 408 || status == 429 || (status >= 500 && status < 600));
            if (!retry){
                break;
            }
        } else {
            ESP_LOGW(TAG, "HTTP POST failed: %s", esp_err_to_name(post_err));
        }

        if (i<2){
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    
    esp_http_client_cleanup(client);
    wifi_disconnect();
    return post_err;
}