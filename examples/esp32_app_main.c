/**
 * @file esp32_app_main.c
 * @brief Reference application entry point for ESP32 Classic (BNEP mode)
 *
 * Demonstrates the required GAP security setup, NVS bonding persistence,
 * and TinyPAN initialization sequence for Bluetooth PAN on ESP-IDF.
 *
 * This file is NOT compiled by the TinyPAN library. It is provided as a
 * reference for integrators.
 */

#include <string.h>
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"

#include "tinypan.h"

static const char* TAG = "app_main";

/* Replace with your phone's Bluetooth MAC address */
static const uint8_t PHONE_BD_ADDR[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};

/* ============================================================================
 * GAP Security Callback
 *
 * TinyPAN does not handle GAP security. The application MUST configure SSP
 * and register this callback before calling tinypan_init().
 * ============================================================================ */

static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                ESP_LOGI(TAG, "Auth complete with %02x:%02x:%02x:%02x:%02x:%02x",
                    param->auth_cmpl.bda[0], param->auth_cmpl.bda[1],
                    param->auth_cmpl.bda[2], param->auth_cmpl.bda[3],
                    param->auth_cmpl.bda[4], param->auth_cmpl.bda[5]);
            } else {
                ESP_LOGE(TAG, "Auth failed, status: %d", param->auth_cmpl.stat);
            }
            break;

        case ESP_BT_GAP_CFM_REQ_EVT:
            /* SSP confirmation: auto-accept (ESP32 has no display for numeric comparison) */
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
            break;

        case ESP_BT_GAP_PIN_REQ_EVT: {
            /* Legacy pairing fallback */
            esp_bt_pin_code_t pin = {'1', '2', '3', '4'};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
            break;
        }

        default:
            break;
    }
}

/* ============================================================================
 * TinyPAN Event Callback
 * ============================================================================ */

static void tinypan_event_handler(tinypan_event_t event, void* user_data) {
    (void)user_data;
    switch (event) {
        case TINYPAN_EVENT_IP_ACQUIRED: {
            tinypan_ip_info_t info;
            if (tinypan_get_ip_info(&info) == TINYPAN_OK) {
                ESP_LOGI(TAG, "IP acquired: " IPSTR, IP2STR(&info.ip_addr));
            }
            break;
        }
        case TINYPAN_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected (auto-reconnect will retry)");
            break;
        default:
            break;
    }
}

/* ============================================================================
 * Application Entry Point
 * ============================================================================ */

void app_main(void) {
    /* 1. NVS must be initialized before Bluedroid so bonding keys persist */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* 2. Initialize Bluetooth controller and Bluedroid */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&bt_cfg);
    esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    esp_bluedroid_init();
    esp_bluedroid_enable();

    /* 3. Configure GAP security (BEFORE TinyPAN init)
     *    Without this, Android/iOS will reject L2CAP with auth failure 0x05. */
    esp_bt_sp_param_t iocap_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;  /* No Input No Output */
    esp_bt_gap_set_security_param(iocap_type, &iocap, sizeof(uint8_t));
    esp_bt_gap_register_callback(gap_callback);

    /* 4. Initialize and start TinyPAN */
    tinypan_config_t config;
    tinypan_config_init(&config);
    memcpy(config.remote_addr, PHONE_BD_ADDR, 6);

    tinypan_init(&config);
    tinypan_set_event_callback(tinypan_event_handler, NULL);
    tinypan_start();

    /* 5. Main loop */
    while (1) {
        tinypan_process();
        vTaskDelay(pdMS_TO_TICKS(tinypan_get_next_timeout_ms()));
    }
}
