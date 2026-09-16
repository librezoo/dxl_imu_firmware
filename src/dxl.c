#include "imu_power/dxl.h"

#include <string.h>

static size_t unstuff(const uint8_t *in, size_t length, uint8_t *out, size_t capacity)
{
    size_t i, n = 0;
    for (i = 0; i < length; ++i) {
        if (n >= capacity) return 0;
        out[n++] = in[i];
        if (n >= 3u && out[n - 3u] == 0xFFu && out[n - 2u] == 0xFFu &&
            out[n - 1u] == 0xFDu) {
            if (i + 1u >= length || in[i + 1u] != 0xFDu) return 0;
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
    /* Protocol 2 length and CRC describe the stuffed wire packet. Only the
       instruction/parameter area is unstuffed, after validating both. */
    declared = (uint16_t)packet[5] | ((uint16_t)packet[6] << 8u);
    if ((size_t)declared + 7u != packet_length || declared < 3u ||
        packet_length > IMU_POWER_DXL_MAX_PACKET) return false;
    received = (uint16_t)packet[packet_length - 2u] |
               ((uint16_t)packet[packet_length - 1u] << 8u);
    calculated = imu_power_dxl_crc16(packet, packet_length - 2u);
    if (received != calculated) return false;
    unstuffed_length = unstuff(&packet[7], declared - 2u, scratch, scratch_size);
    if (unstuffed_length == 0u) return false;
    frame->id = packet[4];
    frame->instruction = scratch[0];
    frame->parameters = &scratch[1];
    frame->parameter_length = unstuffed_length - 1u;
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
    size_t stuffed_length, wire_length;
    if (payload_length > IMU_POWER_DXL_MAX_PACKET - 16u || out == NULL ||
        (payload_length != 0u && payload == NULL) || out_capacity < 11u) return 0;
    if (out_capacity > IMU_POWER_DXL_MAX_PACKET) out_capacity = IMU_POWER_DXL_MAX_PACKET;
    raw[0] = 0x55u; raw[1] = error;
    if (payload_length != 0u) memcpy(&raw[2], payload, payload_length);
    /* Keep the header, length and CRC out of byte stuffing. Reserve the two
       CRC bytes before writing the body, then calculate CRC over wire bytes. */
    stuffed_length = stuff(raw, payload_length + 2u, &out[7], out_capacity - 9u);
    if (stuffed_length == 0u) return 0;
    out[0] = 0xFFu; out[1] = 0xFFu; out[2] = 0xFDu; out[3] = 0u;
    out[4] = id;
    length = (uint16_t)(stuffed_length + 2u);
    out[5] = (uint8_t)length; out[6] = (uint8_t)(length >> 8u);
    wire_length = 7u + stuffed_length;
    crc = imu_power_dxl_crc16(out, wire_length);
    out[wire_length++] = (uint8_t)crc;
    out[wire_length++] = (uint8_t)(crc >> 8u);
    return wire_length;
}

static const uint8_t packet_header[] = {0xFFu, 0xFFu, 0xFDu, 0u};

static void keep_header_suffix(imu_power_dxl_stream_t *stream)
{
    size_t keep = stream->length < sizeof(packet_header) ? stream->length : sizeof(packet_header);
    while (keep != 0u &&
           memcmp(&stream->packet[stream->length - keep], packet_header, keep) != 0) --keep;
    memmove(stream->packet, &stream->packet[stream->length - keep], keep);
    stream->length = keep;
}

bool imu_power_dxl_stream_feed(imu_power_dxl_stream_t *stream, uint8_t byte,
                               imu_power_dxl_frame_t *frame,
                               uint8_t *scratch, size_t scratch_size)
{
    uint16_t declared;
    bool parsed;
    if (stream->length >= sizeof(stream->packet)) stream->length = 0u;
    stream->packet[stream->length++] = byte;
    if (stream->length <= sizeof(packet_header)) {
        /* Keep an overlapping header prefix, e.g. FF FF FF FD 00. */
        keep_header_suffix(stream);
        return false;
    }
    if (stream->length < 7u) return false;
    declared = (uint16_t)stream->packet[5] | ((uint16_t)stream->packet[6] << 8u);
    if (declared < 3u || (size_t)declared + 7u > sizeof(stream->packet)) {
        keep_header_suffix(stream);
        return false;
    }
    if (stream->length == (size_t)declared + 7u) {
        parsed = imu_power_dxl_parse(stream->packet, stream->length, frame, scratch, scratch_size);
        /* Check CRC before looking for a new header: CRC bytes are not stuffed
           and may finish an apparent header at the end of a valid packet. */
        if (parsed) stream->length = 0u;
        else keep_header_suffix(stream);
        return parsed;
    }
    /* Stuffed body data contains FF FF FD FD, never FF FF FD 00. Recover from
       a damaged length without waiting for an idle bus or filling the buffer. */
    if (memcmp(&stream->packet[stream->length - sizeof(packet_header)],
               packet_header, sizeof(packet_header)) == 0) keep_header_suffix(stream);
    return false;
}
