/**
 * @file app_walkie_protocol.c
 * @brief UDP 对讲包格式工具。
 */
#include "app_walkie_protocol.h"

#include "app_business_config.h"
#include "osal_task.h"

#include <string.h>

#define APP_WALKIE_PACKET_MAGIC "WTK1"

static void app_walkie_write_u16(uint8_t *buf, uint16_t value)
{
    buf[0] = (uint8_t)(value & 0xffu);
    buf[1] = (uint8_t)((value >> 8) & 0xffu);
}

static uint16_t app_walkie_read_u16(const uint8_t *buf)
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static void app_walkie_write_u32(uint8_t *buf, uint32_t value)
{
    buf[0] = (uint8_t)(value & 0xffu);
    buf[1] = (uint8_t)((value >> 8) & 0xffu);
    buf[2] = (uint8_t)((value >> 16) & 0xffu);
    buf[3] = (uint8_t)((value >> 24) & 0xffu);
}

static uint32_t app_walkie_read_u32(const uint8_t *buf)
{
    return (uint32_t)buf[0] |
           ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

uint16_t app_walkie_build_packet(uint8_t *out,
                                 uint8_t type,
                                 uint16_t channel,
                                 uint32_t seq,
                                 const char *device_name,
                                 const uint8_t *payload,
                                 uint16_t payload_len)
{
    if (out == NULL || device_name == NULL || payload_len > APP_WALKIE_PACKET_MAX_PAYLOAD) {
        return 0u;
    }

    memcpy(&out[0], APP_WALKIE_PACKET_MAGIC, 4u);
    out[4] = type;
    out[5] = APP_WALKIE_PACKET_HEADER_LEN;
    app_walkie_write_u16(&out[6], channel);
    app_walkie_write_u32(&out[8], seq);
    app_walkie_write_u32(&out[12], osal_get_tick_ms());
    memset(&out[16], 0, APP_WALKIE_DEVICE_FIELD_LEN);
    strncpy((char *)&out[16], device_name, APP_WALKIE_DEVICE_FIELD_LEN - 1u);
    app_walkie_write_u16(&out[32], payload_len);

    if (payload != NULL && payload_len > 0u) {
        memcpy(&out[APP_WALKIE_PACKET_HEADER_LEN], payload, payload_len);
    }

    return (uint16_t)(APP_WALKIE_PACKET_HEADER_LEN + payload_len);
}

int app_walkie_parse_packet(const uint8_t *packet,
                            uint16_t len,
                            app_walkie_packet_view_t *view)
{
    if (packet == NULL || view == NULL || len < APP_WALKIE_PACKET_HEADER_LEN) {
        return -1;
    }
    if (memcmp(packet, APP_WALKIE_PACKET_MAGIC, 4u) != 0) {
        return -2;
    }

    uint8_t header_len = packet[5];
    uint16_t payload_len = app_walkie_read_u16(&packet[32]);
    if (header_len != APP_WALKIE_PACKET_HEADER_LEN ||
        len < (uint16_t)(header_len + payload_len)) {
        return -3;
    }

    view->type = packet[4];
    view->channel = app_walkie_read_u16(&packet[6]);
    view->seq = app_walkie_read_u32(&packet[8]);
    view->payload = &packet[header_len];
    view->payload_len = payload_len;
    view->packet = packet;
    view->packet_len = (uint16_t)(header_len + payload_len);
    return 0;
}

int app_walkie_packet_is_from_device(const uint8_t *packet, const char *device_name)
{
    char name[APP_WALKIE_DEVICE_FIELD_LEN + 1u];

    if (packet == NULL || device_name == NULL) {
        return 0;
    }

    memcpy(name, &packet[16], APP_WALKIE_DEVICE_FIELD_LEN);
    name[APP_WALKIE_DEVICE_FIELD_LEN] = '\0';
    return strncmp(name, device_name, APP_WALKIE_DEVICE_FIELD_LEN) == 0;
}
