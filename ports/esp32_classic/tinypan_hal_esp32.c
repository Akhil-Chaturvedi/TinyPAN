/**
 * @file tinypan_hal_esp32.c
 * @brief Reference Hardware Abstraction Layer for ESP-IDF (Bluetooth Classic)
 * 
 * This file implements the TinyPAN `tinypan_hal.h` interface for an ESP32
 * running the official ESP-IDF framework with the Bluedroid host stack.
 * 
 * Target: Custom PAN Client (BNEP) over Classic Bluetooth (BR/EDR).
 * 
 * @note Thread Safety
 * The ESP-IDF Bluetooth stack executes all callbacks (like `esp_bt_l2cap_cb`)
 * on a dedicated internal FreeRTOS task (`btu_task`). However, TinyPAN is
 * strictly a single-threaded synchronous library.
 * To safely bridge this, this HAL uses a FreeRTOS `QueueHandle_t` to funnel
 * incoming data and events from the BT task into the application task where
 * `tinypan_process()` is polled.

/* ============================================================================
 * HAL Implementation
 * ============================================================================ */

int hal_bt_init(void) {
    if (s_hal_initialized) return 0;

    s_event_queue = xQueueCreate(TINYPAN_ESP_EVENT_QUEUE_SIZE, sizeof(esp_event_msg_t));
    s_rx_queue = xQueueCreate(TINYPAN_ESP_RX_QUEUE_SIZE, sizeof(esp_rx_msg_t));

    /* Initialize the Classic BT L2CAP client */
    esp_err_t ret = esp_bt_l2cap_register_callback(esp_l2cap_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register L2CAP callback: %s", esp_err_to_name(ret));
        return -1;
    }

    ret = esp_bt_l2cap_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init L2CAP: %s", esp_err_to_name(ret));
        return -1;
    }

    s_hal_initialized = true;
    return 0;
}

void hal_bt_deinit(void) {
    if (!s_hal_initialized) return;

    if (s_is_connected) {
        esp_bt_l2cap_close(s_l2cap_handle);
    }
    
    esp_bt_l2cap_deinit();
    
    if (s_event_queue) vQueueDelete(s_event_queue);
    if (s_rx_queue) vQueueDelete(s_rx_queue);
    
    s_hal_initialized = false;
}

int hal_bt_l2cap_connect(const uint8_t* remote_addr, uint16_t local_mtu) {
    (void)local_mtu; /* ESP32 handles MTU internally during init config */
    
    ESP_LOGI(TAG, "Connecting to BDA: %02x:%02x:%02x:%02x:%02x:%02x",
             remote_addr[0], remote_addr[1], remote_addr[2],
             remote_addr[3], remote_addr[4], remote_addr[5]);

    /* Connect to the standard BNEP PSM */
    esp_err_t ret = esp_bt_l2cap_connect((uint8_t*)remote_addr, ESP_BT_PSM_BNEP, 0);
    return (ret == ESP_OK) ? 0 : -1;
}

void hal_bt_l2cap_disconnect(void) {
    if (s_is_connected) {
        esp_bt_l2cap_close(s_l2cap_handle);
    }
}

void hal_bt_l2cap_register_recv_callback(hal_l2cap_recv_callback_t cb, void* user_data) {
    s_recv_cb = cb;
    s_recv_cb_data = user_data;
}

void hal_bt_l2cap_register_event_callback(hal_l2cap_event_callback_t cb, void* user_data) {
    s_event_cb = cb;
    s_event_cb_data = user_data;
}

bool hal_bt_l2cap_can_send(void) {
    /* Currently the ESP-IDF API does not expose a synchronous "can send"
       queue depth monitor for the underlying controller. We assume true. */
    return s_is_connected && !s_tx_busy;
}

void hal_bt_l2cap_request_can_send_now(void) {
    /* Since we pretend it's always ready, just schedule an artificial event */
    if (s_is_connected) {
        esp_event_msg_t msg;
        msg.event_id = HAL_L2CAP_EVENT_CAN_SEND_NOW;
        return (ret == ESP_OK) ? 0 : (ret == ESP_ERR_NO_MEM ? 1 : -1);
    }

    /* Otherwise, we MUST bounce to s_tx_aligned_buf to ensure DMA safety */
    if (p->tot_len > sizeof(s_tx_aligned_buf)) {
        ESP_LOGE(TAG, "TX pbuf too large for bounce buffer: %u", p->tot_len);
        return -1;
    }

    pbuf_copy_partial(p, s_tx_aligned_buf, p->tot_len, 0);
    esp_err_t ret = esp_bt_l2cap_vfs_send(s_l2cap_handle, s_tx_aligned_buf, p->tot_len);
    return (ret == ESP_OK) ? 0 : (ret == ESP_ERR_NO_MEM ? 1 : -1);
}

uint32_t hal_get_tick_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}
