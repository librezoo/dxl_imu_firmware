#include "imu_power/dxl.h"

#include <string.h>

static size_t unstuff(const uint8_t *in, size_t length, uint8_t *out, size_t capacity)
{
    size_t i, n = 0;
    for (i = 0; i < length; ++i) {
        if (n >= capacity) return 0;
        out[n++] = in[i];
        if (n >= 3u && out[n - 3u] == 0xFFu && out[n - 2u] == 0xFFu &&
            out[n - 1u] == 0xFDu && i + 1u < length && in[i + 1u] == 0xFDu) {
            ++i;
        }
    }
    return n;
}

static size_t stuff(const uint8_t *in, size_t length, uint8_t *out, size_t capacity)
{
    size_t i, n = 0;
    for (i = 0; i < length; ++i) {
        if (n >= capacity) return 0;
        out[n++] = in[i];
        if (n >= 3u && out[n - 3u] == 0xFFu && out[n - 2u] == 0xFFu &&
            out[n - 1u] == 0xFDu) {
            if (n >= capacity) return 0;
            out[n++] = 0xFDu;
        }
    }
    return n;
}

uint16_t imu_power_dxl_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0;
    size_t i;
    unsigned bit;
    for (i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8u;
        for (bit = 0; bit < 8u; ++bit) {
            crc = (uint16_t)((crc << 1u) ^ ((crc & 0x8000u) ? 0x8005u : 0u));
        }
    }
    return crc;
}

bool imu_power_dxl_parse(const uint8_t *packet, size_t packet_length,
                         imu_power_dxl_frame_t *frame,
                         uint8_t *scratch, size_t scratch_size)
{
    uint16_t declared, received, calculated;
    size_t unstuffed_length;
    if (packet == NULL || frame == NULL || scratch == NULL || packet_length < 10u ||
        packet[0] != 0xFFu || packet[1] != 0xFFu || packet[2] != 0xFDu || packet[3] != 0u) {
        return false;
    }
    unstuffed_length = unstuff(packet, packet_length, scratch, scratch_size);
    if (unstuffed_length < 10u) return false;
    declared = (uint16_t)scratch[5] | ((uint16_t)scratch[6] << 8u);
    if ((size_t)declared + 7u != unstuffed_length || declared < 3u) return false;
    received = (uint16_t)scratch[unstuffed_length - 2u] |
               ((uint16_t)scratch[unstuffed_length - 1u] << 8u);
    calculated = imu_power_dxl_crc16(scratch, unstuffed_length - 2u);
    if (received != calculated) return false;
    frame->id = scratch[4];
    frame->instruction = scratch[7];
    frame->parameters = &scratch[8];
    frame->parameter_length = declared - 3u;
    frame->broadcast = frame->id == IMU_POWER_DXL_BROADCAST;
    return true;
}

size_t imu_power_dxl_build_status(uint8_t id, uint8_t error,
                                  const uint8_t *payload, size_t payload_length,
                                  uint8_t *out, size_t out_capacity)
{
    uint8_t raw[IMU_POWER_DXL_MAX_PACKET];
    uint16_t length;
    uint16_t crc;
    size_t raw_length, stuffed_length;
    if (payload_length > IMU_POWER_DXL_MAX_PACKET - 16u || out == NULL) return 0;
    raw[0] = 0xFFu; raw[1] = 0xFFu; raw[2] = 0xFDu; raw[3] = 0u;
    raw[4] = id;
    length = (uint16_t)(payload_length + 4u);
    raw[5] = (uint8_t)length; raw[6] = (uint8_t)(length >> 8u);
    raw[7] = 0x55u; raw[8] = error;
    if (payload_length != 0u && payload != NULL) memcpy(&raw[9], payload, payload_length);
    raw_length = 9u + payload_length;
    crc = imu_power_dxl_crc16(raw, raw_length);
    raw[raw_length++] = (uint8_t)crc;
    raw[raw_length++] = (uint8_t)(crc >> 8u);
    if (out_capacity < 9u) return 0;
    memcpy(out, raw, 4u);
    stuffed_length = stuff(&raw[4], raw_length - 4u, &out[4], out_capacity - 4u);
    return stuffed_length == 0u ? 0u : stuffed_length + 4u;
}
