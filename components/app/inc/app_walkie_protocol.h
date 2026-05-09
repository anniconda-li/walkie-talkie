/**
 * @file app_walkie_protocol.h
 * @brief UDP 对讲包格式工具。
 */
#ifndef APP_WALKIE_PROTOCOL_H
#define APP_WALKIE_PROTOCOL_H

#include "app_business_config.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_WALKIE_DEVICE_FIELD_LEN     16u
#define APP_WALKIE_PACKET_HEADER_LEN    34u
#define APP_WALKIE_PACKET_MAX_PAYLOAD   APP_BUSINESS_FRAME_BYTES
#define APP_WALKIE_PACKET_MAX_BYTES     (APP_WALKIE_PACKET_HEADER_LEN + APP_WALKIE_PACKET_MAX_PAYLOAD)

typedef enum {
    APP_WALKIE_PKT_REGISTER = 1,
    APP_WALKIE_PKT_CHANNEL = 2,
    APP_WALKIE_PKT_PTT_START = 3,
    APP_WALKIE_PKT_AUDIO = 4,
    APP_WALKIE_PKT_PTT_STOP = 5,
    APP_WALKIE_PKT_HEARTBEAT = 6,
} app_walkie_packet_type_t;

typedef struct {
    uint8_t type;
    uint16_t channel;
    uint32_t seq;
    const uint8_t *payload;
    uint16_t payload_len;
    const uint8_t *packet;
    uint16_t packet_len;
} app_walkie_packet_view_t;

uint16_t app_walkie_build_packet(uint8_t *out,
                                 uint8_t type,
                                 uint16_t channel,
                                 uint32_t seq,
                                 const char *device_name,
                                 const uint8_t *payload,
                                 uint16_t payload_len);

int app_walkie_parse_packet(const uint8_t *packet,
                            uint16_t len,
                            app_walkie_packet_view_t *view);

int app_walkie_packet_is_from_device(const uint8_t *packet, const char *device_name);

#ifdef __cplusplus
}
#endif

#endif /* APP_WALKIE_PROTOCOL_H */
