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

static size_t make_packet(uint8_t id, uint8_t instruction, const uint8_t *parameters,
                          size_t parameter_length, uint8_t *out)
{
    uint16_t length = (uint16_t)(parameter_length + 3u);
    uint16_t crc;
    size_t n;
    out[0] = 0xFFu; out[1] = 0xFFu; out[2] = 0xFDu; out[3] = 0u;
    out[4] = id; out[5] = (uint8_t)length; out[6] = (uint8_t)(length >> 8u);
    out[7] = instruction;
    memcpy(&out[8], parameters, parameter_length);
    n = 8u + parameter_length;
    crc = imu_power_dxl_crc16(out, n);
    out[n++] = (uint8_t)crc; out[n++] = (uint8_t)(crc >> 8u);
    return n;
}

/* Sends one instruction and returns the DXL error byte (0xFF: no reply). */
static int exchange(imu_power_device_t *device, uint8_t instruction,
                    const uint8_t *parameters, size_t length, uint8_t *payload)
{
    uint8_t packet[128], scratch[128], response[128];
    imu_power_dxl_frame_t frame;
    size_t n = make_packet(200u, instruction, parameters, length, packet);
    size_t r;
    assert(imu_power_dxl_parse(packet, n, &frame, scratch, sizeof(scratch)));
    r = imu_power_device_handle(device, &frame, response, sizeof(response));
    if (r == 0u) return 0xFF;
    if (payload != NULL && r > 11u) memcpy(payload, &response[9], r - 11u);
    return response[8];
}

static int write_bytes(imu_power_device_t *device, uint16_t address, const uint8_t *data, size_t n)
{
    uint8_t p[16];
    p[0] = (uint8_t)address; p[1] = (uint8_t)(address >> 8u);
    memcpy(&p[2], data, n);
    return exchange(device, IMU_POWER_DXL_WRITE, p, n + 2u, NULL);
}

static void command(imu_power_device_t *device, uint8_t value)
{
    assert(write_bytes(device, 197u, &value, 1u) == 0);
}

static void unlock(imu_power_device_t *device)
{
    command(device, IMU_POWER_CONFIG_CMD_UNLOCK);
    command(device, IMU_POWER_CONFIG_CMD_UNLOCK_CONFIRM);
}

int main(void)
{
    imu_power_config_storage_t storage = { slot_read, slot_write, NULL };
    imu_power_device_t device;
    uint8_t payload[16];
    imu_power_config_t config;
    uint32_t generation;

    /* Defaults reproduce the historical +/-500 dps, +/-2 g, 240 Hz setup. */
    memset(slots, 0xFF, sizeof(slots));
    imu_power_device_init(&device, &storage);
    assert(device.active_config.gyro_fs == 2u && device.active_config.accel_fs == 0u);
    assert(device.active_config.imu_odr == 7u && device.active_config.sflp_odr == 4u);
    assert(device.runtime_gyro_fs == 2u && device.runtime_imu_odr == 7u);
    assert(device.diagnostics.firmware_version == IMU_POWER_FW_VERSION);

    /* Parameters are readable without unlock; runtime mirror at 232. */
    assert(exchange(&device, IMU_POWER_DXL_READ, (uint8_t[]){224u, 0u, 4u, 0u}, 4u, payload) == 0);
    assert(memcmp(payload, (uint8_t[]){2u, 0u, 7u, 4u}, 4u) == 0);
    assert(exchange(&device, IMU_POWER_DXL_READ, (uint8_t[]){232u, 0u, 4u, 0u}, 4u, payload) == 0);
    assert(memcmp(payload, (uint8_t[]){2u, 0u, 7u, 4u}, 4u) == 0);

    /* Locked writes are refused. */
    assert(write_bytes(&device, 225u, (uint8_t[]){1u}, 1u) == IMU_POWER_DXL_ACCESS);
    assert(device.active_config.accel_fs == 0u);

    unlock(&device);
    /* Per-byte range checks. */
    assert(write_bytes(&device, 225u, (uint8_t[]){4u}, 1u) == IMU_POWER_DXL_DATA_RANGE);
    assert(write_bytes(&device, 224u, (uint8_t[]){5u}, 1u) == IMU_POWER_DXL_DATA_RANGE);
    assert(write_bytes(&device, 226u, (uint8_t[]){9u}, 1u) == IMU_POWER_DXL_DATA_RANGE);
    assert(write_bytes(&device, 227u, (uint8_t[]){6u}, 1u) == IMU_POWER_DXL_DATA_RANGE);
    assert(write_bytes(&device, 228u, (uint8_t[]){0u}, 1u) == IMU_POWER_DXL_ACCESS);
    assert(device.active_config.accel_fs == 0u && device.active_config.gyro_fs == 2u);

    /* Valid write changes the staged/active value but not the running IMU. */
    assert(write_bytes(&device, 225u, (uint8_t[]){1u}, 1u) == 0);
    assert(device.active_config.accel_fs == 1u && device.runtime_accel_fs == 0u);
    assert(exchange(&device, IMU_POWER_DXL_READ, (uint8_t[]){224u, 0u, 4u, 0u}, 4u, payload) == 0);
    assert(payload[1] == 1u);
    assert(exchange(&device, IMU_POWER_DXL_READ, (uint8_t[]){232u, 0u, 4u, 0u}, 4u, payload) == 0);
    assert(payload[1] == 0u);

    /* Four bytes at once, including 4000 dps and 480 Hz sensors / 240 Hz SFLP. */
    assert(write_bytes(&device, 224u, (uint8_t[]){12u, 3u, 8u, 4u}, 4u) == 0);
    command(&device, IMU_POWER_CONFIG_CMD_SAVE);
    assert(device.diagnostics.config_status == 0u);
    assert(imu_power_config_load(&storage, &config, &generation));
    assert(config.gyro_fs == 12u && config.accel_fs == 3u && config.imu_odr == 8u && config.sflp_odr == 4u);

    /* SFLP faster than the sensors is rejected at save time and nothing is stored. */
    assert(write_bytes(&device, 226u, (uint8_t[]){6u}, 1u) == 0);
    {
        uint8_t save = IMU_POWER_CONFIG_CMD_SAVE;
        assert(write_bytes(&device, 197u, &save, 1u) == IMU_POWER_DXL_RESULT_FAIL);
    }
    assert(imu_power_config_load(&storage, &config, &generation));
    assert(config.imu_odr == 8u);
    /* Fixing the order in two steps works: 120 Hz sensors need 120 Hz SFLP. */
    assert(write_bytes(&device, 227u, (uint8_t[]){3u}, 1u) == 0);
    command(&device, IMU_POWER_CONFIG_CMD_SAVE);
    assert(imu_power_config_load(&storage, &config, &generation));
    assert(config.imu_odr == 6u && config.sflp_odr == 3u && config.gyro_fs == 12u);

    /* A reboot loads the stored values as the running configuration. */
    imu_power_device_init(&device, &storage);
    assert(device.runtime_gyro_fs == 12u && device.runtime_accel_fs == 3u);
    assert(device.runtime_imu_odr == 6u && device.runtime_sflp_odr == 3u);

    /* Defaults command restores the historical values. */
    unlock(&device);
    assert(write_bytes(&device, 225u, (uint8_t[]){3u}, 1u) == 0);
    command(&device, IMU_POWER_CONFIG_CMD_LOAD_DEFAULTS);
    assert(device.active_config.gyro_fs == 2u && device.active_config.sflp_odr == 4u);
    assert(exchange(&device, IMU_POWER_DXL_READ, (uint8_t[]){224u, 0u, 4u, 0u}, 4u, payload) == 0);
    assert(memcmp(payload, (uint8_t[]){2u, 0u, 7u, 4u}, 4u) == 0);
    /* A later single-field write starts from defaults, not stale staged data. */
    assert(write_bytes(&device, 192u, (uint8_t[]){201u}, 1u) == 0);
    assert(device.active_config.device_id == 201u && device.active_config.accel_fs == 0u);

    /* Rate pairs. */
    assert(imu_power_config_valid_rates(7u, 4u));
    assert(imu_power_config_valid_rates(8u, 5u));
    assert(imu_power_config_valid_rates(6u, 3u));
    assert(imu_power_config_valid_rates(6u, 2u));
    assert(!imu_power_config_valid_rates(6u, 4u));
    assert(!imu_power_config_valid_rates(7u, 5u));

    /* A version 1 record keeps ID and baud and gains default IMU settings. */
    {
        imu_power_config_record_v1_t v1;
        memset(slots, 0xFF, sizeof(slots));
        memset(&v1, 0, sizeof(v1));
        v1.magic = IMU_POWER_CONFIG_MAGIC; v1.version = IMU_POWER_CONFIG_VERSION_V1;
        v1.generation = 5u; v1.config.device_id = 9u; v1.config.baudrate = 2000000u;
        v1.crc32 = imu_power_crc32(&v1, offsetof(imu_power_config_record_v1_t, crc32));
        memcpy(&slots[1], &v1, sizeof(v1));
        assert(imu_power_config_load(&storage, &config, &generation));
        assert(generation == 5u && config.device_id == 9u && config.baudrate == 2000000u);
        assert(config.gyro_fs == 2u && config.imu_odr == 7u && config.sflp_odr == 4u);
        /* Saving afterwards writes a version 2 record that loads normally. */
        assert(imu_power_config_save(&storage, &config, generation, &generation));
        assert(generation == 6u);
        assert(imu_power_config_load(&storage, &config, &generation));
        assert(config.device_id == 9u && generation == 6u);
    }

    /* Wire gyro scaling follows the configured full scale. */
    {
        imu_power_sample_t s;
        uint8_t out[IMU_POWER_DATA_LENGTH];
        memset(&s, 0, sizeof(s));
        s.bias_valid = true; s.gyro_raw[0] = 100; s.gyro_bias_125[0] = 400; /* 400 * 4.375 mdps */
        s.gyro_ratio = 4u;              /* 500 dps: 100 counts = 400 units, minus bias = 0 */
        imu_power_pack_sample(&s, out);
        assert(out[0] == 0u && out[1] == 0u);
        s.gyro_ratio = 16u;             /* 2000 dps: 100 counts = 1600 units, minus 400 = 1200 -> 75 */
        imu_power_pack_sample(&s, out);
        assert(out[0] == 75u && out[1] == 0u);
        s.gyro_ratio = 0u;              /* historical default stays 500 dps */
        imu_power_pack_sample(&s, out);
        assert(out[0] == 0u && out[1] == 0u);
    }

    puts("imu_power_param_tests: PASS");
    return 0;
}
