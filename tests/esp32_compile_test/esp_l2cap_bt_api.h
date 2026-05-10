/*
 * Stub esp_l2cap_bt_api.h for ESP32 HAL compilation test.
 *
 * THIS IS THE CRITICAL FILE. Every type, enum value, struct field, and
 * function signature here must match ESP-IDF v5.5.x exactly. If the HAL
 * compiles against this stub, it will compile against the real ESP-IDF.
 *
 * Verified against:
 *   espressif-esp-idf-tree-master-components-bt.txt lines 167260-167500
 */
#ifndef ESP_L2CAP_BT_API_H
#define ESP_L2CAP_BT_API_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_bt.h"

/* ============================================================================
 * Security Constants (from esp_l2cap_bt_api.h)
 * ============================================================================ */

#define ESP_BT_L2CAP_SEC_NONE            0x0000
#define ESP_BT_L2CAP_SEC_AUTHORIZE       0x0001
#define ESP_BT_L2CAP_SEC_AUTHENTICATE    0x0012
#define ESP_BT_L2CAP_SEC_ENCRYPT         0x0024

/* Control flags type */
typedef uint32_t esp_bt_l2cap_cntl_flags_t;

/* ============================================================================
 * Status type
 * ============================================================================ */

typedef enum {
    ESP_BT_L2CAP_SUCCESS  = 0,
    ESP_BT_L2CAP_FAILURE  = 1,
} esp_bt_l2cap_status_t;

/* ============================================================================
 * Event enum (from esp_bt_l2cap_cb_event_t)
 * ============================================================================ */

typedef enum {
    ESP_BT_L2CAP_INIT_EVT           = 0,
    ESP_BT_L2CAP_UNINIT_EVT         = 1,
    ESP_BT_L2CAP_OPEN_EVT           = 16,
    ESP_BT_L2CAP_CLOSE_EVT          = 17,
    ESP_BT_L2CAP_START_EVT          = 18,
    ESP_BT_L2CAP_CL_INIT_EVT        = 19,
    ESP_BT_L2CAP_SRV_STOP_EVT       = 36,
    ESP_BT_L2CAP_VFS_REGISTER_EVT   = 38,
    ESP_BT_L2CAP_VFS_UNREGISTER_EVT = 39,
} esp_bt_l2cap_cb_event_t;

/* ============================================================================
 * Callback parameter union (from esp_bt_l2cap_cb_param_t)
 *
 * Uses anonymous structs inside the union — this matches the real ESP-IDF
 * header where struct member names (open, close, etc.) are in the union
 * member namespace and do NOT conflict with POSIX function names.
 * ============================================================================ */

typedef union {
    /* INIT_EVT params */
    struct {
        esp_bt_l2cap_status_t status;
    } init;

    /* UNINIT_EVT params */
    struct {
        esp_bt_l2cap_status_t status;
    } uninit;

    /* OPEN_EVT params — THE CRITICAL ONE */
    struct {
        esp_bt_l2cap_status_t  status;
        uint32_t               handle;    /* Connection handle */
        int                    fd;        /* FILE DESCRIPTOR for VFS I/O */
        esp_bd_addr_t          rem_bda;   /* Remote address */
        int32_t                tx_mtu;    /* Transmit MTU */
    } open;

    /* CLOSE_EVT params */
    struct {
        esp_bt_l2cap_status_t  status;
        uint32_t               handle;
        bool                   async;
    } close;

    /* START_EVT params */
    struct {
        esp_bt_l2cap_status_t  status;
        uint32_t               handle;
    } start;

    /* CL_INIT_EVT params — connection initiated, NOT established */
    struct {
        esp_bt_l2cap_status_t  status;
        uint32_t               handle;
        uint8_t                sec_id;
    } cl_init;

    /* SRV_STOP_EVT params */
    struct {
        esp_bt_l2cap_status_t  status;
    } srv_stop;

    /* VFS_REGISTER_EVT params */
    struct {
        esp_bt_l2cap_status_t  status;
    } vfs_register;

    /* VFS_UNREGISTER_EVT params */
    struct {
        esp_bt_l2cap_status_t  status;
    } vfs_unregister;

} esp_bt_l2cap_cb_param_t;

/* ============================================================================
 * Callback type
 * ============================================================================ */

typedef void (*esp_bt_l2cap_cb_t)(esp_bt_l2cap_cb_event_t event,
                                   esp_bt_l2cap_cb_param_t *param);

/* ============================================================================
 * Protocol status
 * ============================================================================ */

typedef struct {
    bool  connected;
    int   fd;
} esp_bt_l2cap_protocol_status_t;

/* ============================================================================
 * API Functions — signatures must match ESP-IDF exactly
 * ============================================================================ */

/* Initialization */
esp_err_t esp_bt_l2cap_register_callback(esp_bt_l2cap_cb_t callback);
esp_err_t esp_bt_l2cap_init(void);
esp_err_t esp_bt_l2cap_deinit(void);

/* VFS registration (REQUIRED before data I/O) */
esp_err_t esp_bt_l2cap_vfs_register(void);
esp_err_t esp_bt_l2cap_vfs_unregister(void);

/* Connection — 3 args: cntl_flag, remote_psm, peer_bd_addr */
esp_err_t esp_bt_l2cap_connect(esp_bt_l2cap_cntl_flags_t cntl_flag,
                                uint16_t remote_psm,
                                esp_bd_addr_t peer_bd_addr);

/* Server */
esp_err_t esp_bt_l2cap_start_srv(esp_bt_l2cap_cntl_flags_t cntl_flag, uint16_t local_psm);
esp_err_t esp_bt_l2cap_stop_all_srv(void);
esp_err_t esp_bt_l2cap_stop_srv(uint16_t local_psm);

/* Status */
esp_err_t esp_bt_l2cap_get_protocol_status(esp_bt_l2cap_protocol_status_t *status);

#endif /* ESP_L2CAP_BT_API_H */
