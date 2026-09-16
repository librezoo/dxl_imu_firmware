#include "imu_power/config.h"
#include "imu_power/device.h"
#include "imu_power/dxl.h"
#include "imu_power/imu_wire.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static imu_power_config_record_t slots[2];
static bool slot_read(void *ctx, unsigned slot, void *dst, size_t size)
{
    (void)ctx;
    if (slot >= 2u || size != sizeof(slots[slot])) return false;
    memcpy(dst, &slots[slot], size);
    return true;
}
static bool slot_write(void *ctx, unsigned slot, const void *src, size_t size)
{
    (void)ctx;
    if (slot >= 2u || size != sizeof(slots[slot])) return false;
    memcpy(&slots[slot], src, size);
    return true;
}

static size_t make_packet(uint8_t id, uint8_t instruction,
                         const uint8_t *parameters, size_t parameter_length,
                         uint8_t *out, size_t capacity)
{
    uint8_t raw[IMU_POWER_DXL_MAX_PACKET];
    uint16_t length = (uint16_t)(parameter_length + 3u);
    uint16_t crc;
    size_t n;
    raw[0] = 0xFFu; raw[1] = 0xFFu; raw[2] = 0xFDu; raw[3] = 0u;
    raw[4] = id; raw[5] = (uint8_t)length; raw[6] = (uint8_t)(length >> 8u);
    raw[7] = instruction;
    memcpy(&raw[8], parameters, parameter_length);
    n = 8u + parameter_length;
    crc = imu_power_dxl_crc16(raw, n);
    raw[n++] = (uint8_t)crc; raw[n++] = (uint8_t)(crc >> 8u);
    assert(capacity >= n);
    memcpy(out, raw, n);
    return n;
}

int main(void)
{
    imu_power_config_storage_t storage = { slot_read, slot_write, NULL };
    imu_power_device_t device;
    imu_power_dxl_frame_t frame;
    imu_power_sample_t sample = {
        .gyro_raw = {1, -2, 3}, .quat_half = {0x3800u, 0x0000u, 0xb800u},
        .sample_counter = 7u, .age_us = 1000u, .valid = true
    };
    uint8_t packet[128], scratch[128], response[128], parameters[32];
    size_t packet_length, response_length;

    memset(slots, 0xFF, sizeof(slots));
    imu_power_device_init(&device, &storage);
    assert(device.active_config.device_id == 200u);
    assert(device.active_config.baudrate == 1000000u);
    imu_power_device_set_sample(&device, &sample);

    packet_length = make_packet(200u, IMU_POWER_DXL_READ,
                                (uint8_t[]){124u, 0u, 12u, 0u}, 4u,
                                packet, sizeof(packet));
    assert(imu_power_dxl_parse(packet, packet_length, &frame, scratch, sizeof(scratch)));
    response_length = imu_power_device_handle(&device, &frame, response, sizeof(response));
    assert(response_length != 0u);
    assert(response[4] == 200u && response[7] == 0x55u && response[8] == 0u);

    parameters[0] = 124u; parameters[1] = 0u; parameters[2] = 12u; parameters[3] = 0u;
    parameters[4] = 200u; parameters[5] = 20u;
    packet_length = make_packet(0xFEu, IMU_POWER_DXL_SYNC_READ, parameters, 6u,
                                packet, sizeof(packet));
    assert(imu_power_dxl_parse(packet, packet_length, &frame, scratch, sizeof(scratch)));
    response_length = imu_power_device_handle(&device, &frame, response, sizeof(response));
    assert(response_length != 0u);
    assert(response[4] == 200u);

    {
        uint8_t stuffed_payload[] = {0xFFu, 0xFFu, 0xFDu};
        uint8_t stuffed_packet[64];
        size_t stuffed_length = imu_power_dxl_build_status(200u, 0u, stuffed_payload,
                                                            sizeof(stuffed_payload),
                                                            stuffed_packet, sizeof(stuffed_packet));
        /* Header/reserved/ID/length (7), status/error (2), payload, CRC (2),
           plus one inserted byte for this payload's FF FF FD sequence. */
        assert(stuffed_length == 11u + sizeof(stuffed_payload) + 1u);
        assert(memcmp(&stuffed_packet[9], (uint8_t[]){0xFFu, 0xFFu, 0xFDu, 0xFDu}, 4u) == 0);
        assert(imu_power_dxl_parse(stuffed_packet, stuffed_length, &frame,
                                    scratch, sizeof(scratch)));
        assert(frame.parameter_length == 1u + sizeof(stuffed_payload));
        assert(frame.parameters[0] == 0u);
        assert(memcmp(&frame.parameters[1], stuffed_payload, sizeof(stuffed_payload)) == 0);
    }

    /* Maintenance unlock is a two-step sequence; ordinary servo sync writes
       cannot reach the IMU sample block. */
    packet_length = make_packet(200u, IMU_POWER_DXL_WRITE,
                                (uint8_t[]){197u, 0u, IMU_POWER_CONFIG_CMD_UNLOCK}, 3u,
                                packet, sizeof(packet));
    assert(imu_power_dxl_parse(packet, packet_length, &frame, scratch, sizeof(scratch)));
    assert(imu_power_device_handle(&device, &frame, response, sizeof(response)) != 0u);
    packet_length = make_packet(200u, IMU_POWER_DXL_WRITE,
                                (uint8_t[]){197u, 0u, IMU_POWER_CONFIG_CMD_UNLOCK_CONFIRM}, 3u,
                                packet, sizeof(packet));
    assert(imu_power_dxl_parse(packet, packet_length, &frame, scratch, sizeof(scratch)));
    assert(imu_power_device_handle(&device, &frame, response, sizeof(response)) != 0u);
    packet_length = make_packet(200u, IMU_POWER_DXL_WRITE,
                                (uint8_t[]){192u, 0u, 201u}, 3u,
                                packet, sizeof(packet));
    assert(imu_power_dxl_parse(packet, packet_length, &frame, scratch, sizeof(scratch)));
    assert(imu_power_device_handle(&device, &frame, response, sizeof(response)) != 0u);
    assert(device.active_config.device_id == 201u);
    assert(device.runtime_id == 200u);
    sample.gyro_raw[0] = 77;
    imu_power_device_set_sample(&device, &sample);
    packet_length = make_packet(0xFEu, IMU_POWER_DXL_SYNC_WRITE,
                                (uint8_t[]){124u, 0u, 12u, 0u, 201u,
                                            0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
                                            0u, 0u, 0u, 0u, 0u, 0u}, 18u,
                                packet, sizeof(packet));
    assert(imu_power_dxl_parse(packet, packet_length, &frame, scratch, sizeof(scratch)));
    assert(imu_power_device_handle(&device, &frame, response, sizeof(response)) == 0u);
    assert(device.sample.gyro_raw[0] == 77);

    {
        imu_power_config_t saved = {42u, 2000000u, 0u};
        imu_power_config_t loaded;
        uint32_t generation;
        assert(imu_power_config_save(&storage, &saved, 0u, &generation));
        assert(generation == 1u);
        assert(imu_power_config_load(&storage, &loaded, &generation));
        assert(loaded.device_id == 42u && loaded.baudrate == 2000000u);
        slots[1].crc32 ^= 1u;
        assert(!imu_power_config_load(&storage, &loaded, &generation));
        assert(loaded.device_id == 200u && loaded.baudrate == 1000000u);
    }

    assert(imu_power_quaternion_valid(sample.quat_half));
    sample.quat_half[0] = 0x7C00u;
    assert(!imu_power_quaternion_valid(sample.quat_half));
    assert(imu_power_float_to_half(1.0f) == 0x3C00u);
    puts("imu_power_core_tests: PASS");
    return 0;
}
