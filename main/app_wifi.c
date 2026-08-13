/**
 * @file    app_wifi.c
 * @brief   WiFi SoftAP + NVS 配置 实现
 *
 * 启动顺序:
 *   1. nvs_flash_init()
 *   2. esp_netif_init() + esp_event_loop_create_default()
 *   3. esp_netif_create_default_wifi_ap()
 *   4. esp_wifi_init() → set_mode(AP) → set_config(AP) → start()
 */

#include "app_wifi.h"
#include "app_core.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "wifi";

/* ---- NVS 配置键 ---- */
#define NVS_NAMESPACE   "smarthome"
#define KEY_AP_SSID     "ap_ssid"
#define KEY_AP_PWD      "ap_pwd"
#define KEY_MQTT_IP     "mqtt_ip"
#define KEY_MQTT_PORT   "mqtt_port"
#define KEY_MQTT_CLIENT "mqtt_cid"

/* ---- WiFi 事件回调 ---- */
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *evt = event_data;
        ESP_LOGI(TAG, "Station " MACSTR " connected, AID=%d",
                 MAC2STR(evt->mac), evt->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *evt = event_data;
        ESP_LOGI(TAG, "Station " MACSTR " disconnected, AID=%d",
                 MAC2STR(evt->mac), evt->aid);
    }
}

/* ================================================================
 * NVS 配置初始化
 * ================================================================ */
esp_err_t config_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS 分区被截断或损坏 → 擦除后重试 */
        ESP_LOGW(TAG, "NVS corrupted, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 打开命名空间，写入默认配置（仅当键不存在时） */
    nvs_handle_t handle;
    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* 检查 ap_ssid 是否存在 → 不存在则写全套默认值 */
    size_t len;
    if (nvs_get_str(handle, KEY_AP_SSID, NULL, &len) != ESP_OK) {
        ESP_LOGI(TAG, "First boot — writing default config to NVS");

        nvs_set_str(handle, KEY_AP_SSID, DEFAULT_AP_SSID);
        nvs_set_str(handle, KEY_AP_PWD,  DEFAULT_AP_PASSWORD);
        nvs_set_str(handle, KEY_MQTT_IP, DEFAULT_MQTT_BROKER_IP);
        nvs_set_u16(handle, KEY_MQTT_PORT, DEFAULT_MQTT_PORT);
        nvs_set_str(handle, KEY_MQTT_CLIENT, DEFAULT_MQTT_CLIENT_ID);

        nvs_commit(handle);
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "NVS config ready");
    return ESP_OK;
}

/* ================================================================
 * WiFi SoftAP 初始化
 * ================================================================ */
esp_err_t wifi_init_ap(void)
{
    /* 1. 网络栈初始化 (仅首次) */
    static bool netif_inited = false;
    if (!netif_inited) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_ap();
        netif_inited = true;
    }

    /* 2. WiFi 初始化 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 3. 注册事件回调 */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL, NULL));

    /* 4. 从 NVS 读取 SSID/密码 */
    nvs_handle_t handle;
    char ssid[32]     = DEFAULT_AP_SSID;
    char password[64] = DEFAULT_AP_PASSWORD;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(ssid);
        nvs_get_str(handle, KEY_AP_SSID, ssid, &len);
        len = sizeof(password);
        nvs_get_str(handle, KEY_AP_PWD, password, &len);
        nvs_close(handle);
    }

    /* 5. 配置 AP */
    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = 0,          /* 自动判断长度 */
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_config.ap.ssid, ssid, 32);
    strncpy((char *)wifi_config.ap.password, password, 64);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started: SSID=\"%s\"", ssid);
    return ESP_OK;
}
