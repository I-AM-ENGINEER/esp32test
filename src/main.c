#include <stdlib.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "esp_http_server.h"
#include "esp_wifi.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_spiffs.h"

#define DEFAULT_ESP_WIFI_SSID      "ESP32AP"
#define DEFAULT_ESP_WIFI_PASS      "12345678"
#define DEFAULT_MAX_STA_CONN       4

static const char *TAG = "ESP";
static const char *TAG_SPIFFS = "SSPIFS";

#define EXAMPLE_HTTP_QUERY_KEY_MAX_LEN  (64)

static esp_err_t send_file(httpd_req_t *req, char* file_path) {
    FILE* index_html_file = fopen(file_path, "r");
    fseek(index_html_file, 0L, SEEK_END);
    size_t file_length = ftell(index_html_file);
    rewind(index_html_file);
    char* file_buffer = malloc(file_length);
    if(file_buffer == NULL){
        ESP_LOGI(TAG_SPIFFS, "Failed allocate memory for file");
        fclose(index_html_file);
        return ESP_ERR_NO_MEM;
    }
    fread(file_buffer, file_length, 1, index_html_file);
    httpd_resp_send(req, file_buffer, file_length);
    free(file_buffer);
    fclose(index_html_file);
    return ESP_OK;
}

static esp_err_t main_page_get_handler(httpd_req_t *req) {
    ESP_LOGI(TAG_SPIFFS, "GET URI: '%s'", req->uri);
    char* uri_additional = strchr(req->uri, '?');
    
    if(uri_additional == NULL){
        send_file(req, "/spiffs/index.html");
    }else if(uri_additional[1] == 'u'){
        static int i = 0;
        char str_buf[20];
        sprintf(str_buf, "%d", i++);
        httpd_resp_send(req, str_buf, -1);
    }
    return ESP_OK;
}

static esp_err_t main_page_post_handler(httpd_req_t *req) {
    ESP_LOGI(TAG_SPIFFS, "POST URI: '%s'", req->uri);
    char buf[100];
    int ret, remaining = req->content_len;
    
    while (remaining > 0) {
        size_t ggg = sizeof(buf);
        if(remaining < ggg){
            ggg = remaining;
        }
        /* Read the data for the request */
        if ((ret = httpd_req_recv(req, buf, ggg)) <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry receiving if timeout occurred */
                continue;
            }
            return ESP_FAIL;
        }

        /* Send back the same data */
        httpd_resp_send_chunk(req, buf, ret);
        remaining -= ret;

        /* Log data received */
        ESP_LOGI(TAG, "=========== RECEIVED DATA ==========");
        ESP_LOGI(TAG, "%.*s", ret, buf);
        ESP_LOGI(TAG, "====================================");
    }

    // End response
    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

static const httpd_uri_t default_page_get = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = main_page_get_handler,
    .user_ctx  = "хуй"
};

static const httpd_uri_t default_page_post = {
    .uri       = "/",
    .method    = HTTP_POST,
    .handler   = main_page_post_handler,
    .user_ctx  = NULL
};


static httpd_handle_t *start_webserver( void ) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.lru_purge_enable = true;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &default_page_get);
        httpd_register_uri_handler(server, &default_page_post);
        //httpd_register_err_handler(req->handle, HTTPD_404_NOT_FOUND, NULL);
        //httpd_register_basic_auth(server);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

static esp_err_t stop_webserver(httpd_handle_t server) {
    return httpd_stop(server);
}

static void disconnect_handler(void* arg, esp_event_base_t event_base, 
                               int32_t event_id, void* event_data) {
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server) {
        ESP_LOGI(TAG, "Stopping webserver");
        stop_webserver(*server);
        *server = NULL;
    }
}

static void connect_handler(void* arg, esp_event_base_t event_base, 
                            int32_t event_id, void* event_data) {
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) {
        ESP_LOGI(TAG, "Starting webserver");
        *server = start_webserver();
    }
}




esp_netif_t *wifi_ap_start( void ) {
    esp_netif_t *esp_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_config_t wifi_ap_config = {
        .ap = {
            .ssid = DEFAULT_ESP_WIFI_SSID,
            .ssid_len = strlen(DEFAULT_ESP_WIFI_SSID),
            .channel = 1,
            .password = DEFAULT_ESP_WIFI_PASS,
            .max_connection = DEFAULT_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    if (strlen(DEFAULT_ESP_WIFI_PASS) == 0) {
        wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));

    return esp_netif_ap;
}

static esp_err_t spiffs_init( void ){
    ESP_LOGI(TAG, "Initializing SPIFFS");
    
    esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 32,
      .format_if_mount_failed = true
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s). Formatting...", esp_err_to_name(ret));
        esp_spiffs_format(conf.partition_label);
        return ESP_FAIL; 
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    // Check consistency of reported partiton size info.
    if (used > total) {
        ESP_LOGW(TAG, "Number of used bytes cannot be larger than total. Performing SPIFFS_check().");
        ret = esp_spiffs_check(conf.partition_label);
        // Could be also used to mend broken files, to clean unreferenced pages, etc.
        // More info at https://github.com/pellepl/spiffs/wiki/FAQ#powerlosses-contd-when-should-i-run-spiffs_check
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPIFFS_check() failed (%s)", esp_err_to_name(ret));
            return ESP_FAIL;
        } else {
            ESP_LOGI(TAG, "SPIFFS_check() successful");
        }
    }
    return ESP_OK;
}

void app_main() {
    static httpd_handle_t server = NULL;



    ESP_ERROR_CHECK(nvs_flash_init());
    
    ESP_ERROR_CHECK(spiffs_init());    

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    esp_netif_t *esp_netif_ap = wifi_ap_start();
    ESP_ERROR_CHECK(esp_wifi_start() );

    server = start_webserver();
}

