/*
 * TinyPAN Linux/BlueZ HAL
 * 
 * Uses BlueZ L2CAP sockets for Bluetooth communication.
 * This is the real HAL for Linux systems.
 */

#include "../../include/tinypan_hal.h"
#include "../../include/tinypan_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

/* BlueZ includes */
#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

/* ============================================================================
 * State
 * ============================================================================ */

static int s_l2cap_socket = -1;
static int s_hci_socket = -1;
static int s_hci_dev_id = -1;
static bdaddr_t s_local_addr;
static bdaddr_t s_remote_addr;

static hal_l2cap_recv_callback_t s_recv_callback = NULL;
static void* s_recv_callback_user_data = NULL;

static hal_l2cap_event_callback_t s_event_callback = NULL;
static void* s_event_callback_user_data = NULL;

static volatile int s_connecting = 0;
static volatile int s_connected = 0;

/* Receive buffer */
#define RX_BUFFER_SIZE 2048
static uint8_t s_rx_buffer[RX_BUFFER_SIZE];

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/**
 * @brief Set socket to non-blocking mode
 */
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * @brief Get local Bluetooth adapter address
 */
static int get_local_address(int dev_id, bdaddr_t* addr) {
    struct hci_dev_info di;
    int sock;
    
    sock = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
    if (sock < 0) {
        TINYPAN_LOG_ERROR("Failed to open HCI socket: %s", strerror(errno));
        return -1;
    }
    
    memset(&di, 0, sizeof(di));
    di.dev_id = dev_id;
    
    if (ioctl(sock, HCIGETDEVINFO, &di) < 0) {
        TINYPAN_LOG_ERROR("Failed to get device info: %s", strerror(errno));
        close(sock);
        return -1;
    }
    
    bacpy(addr, &di.bdaddr);
    close(sock);
    
    return 0;
}

/* ============================================================================
 * HAL Implementation
 * ============================================================================ */

int hal_bt_init(void) {
    TINYPAN_LOG_INFO("[BlueZ] Initializing...");
    
    /* Find first available Bluetooth adapter */
    s_hci_dev_id = hci_get_route(NULL);
    if (s_hci_dev_id < 0) {
        TINYPAN_LOG_ERROR("[BlueZ] No Bluetooth adapter found");
        return -1;
    }
    
    TINYPAN_LOG_INFO("[BlueZ] Using adapter hci%d", s_hci_dev_id);
    
    /* Get local address */
    if (get_local_address(s_hci_dev_id, &s_local_addr) < 0) {
        TINYPAN_LOG_ERROR("[BlueZ] Failed to get local address");
        return -1;
    }
    
    char addr_str[18];
    ba2str(&s_local_addr, addr_str);
    TINYPAN_LOG_INFO("[BlueZ] Local address: %s", addr_str);
    
    s_l2cap_socket = -1;
    s_connected = 0;
    s_connecting = 0;
    
    return 0;
}

void hal_bt_deinit(void) {
    TINYPAN_LOG_INFO("[BlueZ] De-initializing...");
    
    if (s_l2cap_socket >= 0) {
        close(s_l2cap_socket);
        s_l2cap_socket = -1;
    }
    
    s_connected = 0;
    s_connecting = 0;
}

int hal_bt_l2cap_connect(const uint8_t remote_addr[HAL_BD_ADDR_LEN], uint16_t psm) {
    struct sockaddr_l2 addr;
    int err;
    
    if (s_l2cap_socket >= 0) {
        TINYPAN_LOG_WARN("[BlueZ] Already connected, disconnecting first");
        close(s_l2cap_socket);
    }
    
    /* Store remote address (BlueZ uses reversed byte order) */
    for (int i = 0; i < 6; i++) {
        s_remote_addr.b[5-i] = remote_addr[i];
    }
    
    char addr_str[18];
    ba2str(&s_remote_addr, addr_str);
    TINYPAN_LOG_INFO("[BlueZ] Connecting to %s PSM=0x%04X", addr_str, psm);
    
    /* Create L2CAP socket */
    s_l2cap_socket = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (s_l2cap_socket < 0) {
        TINYPAN_LOG_ERROR("[BlueZ] Failed to create L2CAP socket: %s", strerror(errno));
        return -1;
    }
    
    /* Set socket options for BNEP */
    struct l2cap_options opts;
    socklen_t optlen = sizeof(opts);
    
    if (getsockopt(s_l2cap_socket, SOL_L2CAP, L2CAP_OPTIONS, &opts, &optlen) == 0) {
        opts.imtu = HAL_BNEP_MIN_MTU;
        opts.omtu = HAL_BNEP_MIN_MTU;
        setsockopt(s_l2cap_socket, SOL_L2CAP, L2CAP_OPTIONS, &opts, sizeof(opts));
    }
    
    /* Bind to local adapter */
    memset(&addr, 0, sizeof(addr));
    addr.l2_family = AF_BLUETOOTH;
    bacpy(&addr.l2_bdaddr, &s_local_addr);
    addr.l2_psm = 0;  /* Dynamic for client */
    
    if (bind(s_l2cap_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        TINYPAN_LOG_ERROR("[BlueZ] Bind failed: %s", strerror(errno));
        close(s_l2cap_socket);
        s_l2cap_socket = -1;
        return -1;
    }
    
    /* Set non-blocking for async connect */
    set_nonblocking(s_l2cap_socket);
    
    /* Connect to remote device */
    memset(&addr, 0, sizeof(addr));
    addr.l2_family = AF_BLUETOOTH;
    bacpy(&addr.l2_bdaddr, &s_remote_addr);
    addr.l2_psm = htobs(psm);
    
    s_connecting = 1;
    
    err = connect(s_l2cap_socket, (struct sockaddr*)&addr, sizeof(addr));
    if (err < 0) {
        if (errno == EINPROGRESS) {
            /* Connection in progress - this is normal for non-blocking */
            TINYPAN_LOG_DEBUG("[BlueZ] Connection in progress...");
            return 0;
        }
        
        TINYPAN_LOG_ERROR("[BlueZ] Connect failed: %s", strerror(errno));
        close(s_l2cap_socket);
        s_l2cap_socket = -1;
        s_connecting = 0;
        return -1;
    }
    
    /* Connected immediately (rare) */
    s_connecting = 0;
    s_connected = 1;
    TINYPAN_LOG_INFO("[BlueZ] Connected!");
    
    if (s_event_callback) {
        s_event_callback(HAL_L2CAP_EVENT_CONNECTED, 0, s_event_callback_user_data);
    }
    
    return 0;
}

void hal_bt_l2cap_disconnect(void) {
    if (s_l2cap_socket >= 0) {
        TINYPAN_LOG_INFO("[BlueZ] Disconnecting...");
        close(s_l2cap_socket);
        s_l2cap_socket = -1;
    }
    
    s_connected = 0;
    s_connecting = 0;
}

int hal_bt_l2cap_send(const uint8_t* data, uint16_t len) {
    if (s_l2cap_socket < 0 || !s_connected) {
        return -1;
    }
    
    ssize_t sent = send(s_l2cap_socket, data, len, 0);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 1;  /* Would block, try again */
        }
        TINYPAN_LOG_ERROR("[BlueZ] Send failed: %s", strerror(errno));
        return -1;
    }
    
    TINYPAN_LOG_DEBUG("[BlueZ] Sent %zd bytes", sent);
    return 0;
}

bool hal_bt_l2cap_can_send(void) {
    if (s_l2cap_socket < 0 || !s_connected) {
        return false;
    }
    
    struct pollfd pfd = {
        .fd = s_l2cap_socket,
        .events = POLLOUT
    };
    
    int ret = poll(&pfd, 1, 0);
    return (ret > 0 && (pfd.revents & POLLOUT));
}

void hal_bt_l2cap_request_can_send_now(void) {
    /* In polling mode, the main loop will handle this */
}

void hal_bt_l2cap_register_recv_callback(hal_l2cap_recv_callback_t callback, void* user_data) {
    s_recv_callback = callback;
    s_recv_callback_user_data = user_data;
}

void hal_bt_l2cap_register_event_callback(hal_l2cap_event_callback_t callback, void* user_data) {
    s_event_callback = callback;
    s_event_callback_user_data = user_data;
}

void hal_get_local_bd_addr(uint8_t addr[HAL_BD_ADDR_LEN]) {
    /* Convert BlueZ bdaddr_t to our format (reversed) */
    for (int i = 0; i < 6; i++) {
        addr[i] = s_local_addr.b[5-i];
    }
}

uint32_t hal_get_tick_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int hal_nv_load(const char* key, uint8_t* buffer, uint16_t max_len) {
    (void)key; (void)buffer; (void)max_len;
    return -1;  /* Not implemented */
}

int hal_nv_save(const char* key, const uint8_t* data, uint16_t len) {
    (void)key; (void)data; (void)len;
    return -1;  /* Not implemented */
}

/* ============================================================================
 * Polling Function (call from main loop)
 * ============================================================================ */

/**
 * @brief Poll for events - must be called regularly
 * 
 * This checks for:
 * - Connection completion
 * - Incoming data
 * - Disconnect events
 */
void hal_bt_poll(void) {
    if (s_l2cap_socket < 0) {
        return;
    }
    
    struct pollfd pfd = {
        .fd = s_l2cap_socket,
        .events = POLLIN | POLLOUT | POLLERR | POLLHUP
    };
    
    int ret = poll(&pfd, 1, 0);  /* Non-blocking poll */
    if (ret <= 0) {
        return;
    }
    
    /* Check for errors or hangup */
    if (pfd.revents & (POLLERR | POLLHUP)) {
        TINYPAN_LOG_WARN("[BlueZ] Socket error/hangup");
        
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(s_l2cap_socket, SOL_SOCKET, SO_ERROR, &err, &len);
        
        if (s_connecting) {
            s_connecting = 0;
            if (s_event_callback) {
                s_event_callback(HAL_L2CAP_EVENT_CONNECT_FAILED, err, 
                                 s_event_callback_user_data);
            }
        } else if (s_connected) {
            s_connected = 0;
            if (s_event_callback) {
                s_event_callback(HAL_L2CAP_EVENT_DISCONNECTED, err,
                                 s_event_callback_user_data);
            }
        }
        
        close(s_l2cap_socket);
        s_l2cap_socket = -1;
        return;
    }
    
    /* Check for connection completion */
    if (s_connecting && (pfd.revents & POLLOUT)) {
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(s_l2cap_socket, SOL_SOCKET, SO_ERROR, &err, &len);
        
        if (err == 0) {
            s_connecting = 0;
            s_connected = 1;
            TINYPAN_LOG_INFO("[BlueZ] Connected!");
            
            if (s_event_callback) {
                s_event_callback(HAL_L2CAP_EVENT_CONNECTED, 0,
                                 s_event_callback_user_data);
            }
        } else {
            s_connecting = 0;
            TINYPAN_LOG_ERROR("[BlueZ] Connection failed: %s", strerror(err));
            
            if (s_event_callback) {
                s_event_callback(HAL_L2CAP_EVENT_CONNECT_FAILED, err,
                                 s_event_callback_user_data);
            }
            
            close(s_l2cap_socket);
            s_l2cap_socket = -1;
        }
    }
    
    /* Check for incoming data */
    if (s_connected && (pfd.revents & POLLIN)) {
        ssize_t len = recv(s_l2cap_socket, s_rx_buffer, sizeof(s_rx_buffer), 0);
        
        if (len > 0) {
            TINYPAN_LOG_DEBUG("[BlueZ] Received %zd bytes", len);
            
            if (s_recv_callback) {
                s_recv_callback(s_rx_buffer, (uint16_t)len, s_recv_callback_user_data);
            }
        } else if (len == 0) {
            /* Connection closed */
            TINYPAN_LOG_INFO("[BlueZ] Connection closed by peer");
            s_connected = 0;
            
            if (s_event_callback) {
                s_event_callback(HAL_L2CAP_EVENT_DISCONNECTED, 0,
                                 s_event_callback_user_data);
            }
            
            close(s_l2cap_socket);
            s_l2cap_socket = -1;
        }
    }
}
