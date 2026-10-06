#include "imu_power/config.h"

#include <string.h>

static bool record_valid(const imu_power_config_record_t *record)
{
    if (record->magic != IMU_POWER_CONFIG_MAGIC ||
        record->version != IMU_POWER_CONFIG_VERSION ||
        !imu_power_config_valid(&record->config)) {
        return false;
    }
    return imu_power_crc32(record, offsetof(imu_power_config_record_t, crc32)) == record->crc32;
}

/* Reads a stored version 1 record and upgrades it with default IMU settings. */
static bool record_v1_upgrade(const imu_power_config_record_t *raw,
                              imu_power_config_record_t *out)
{
    imu_power_config_record_v1_t v1;
    memcpy(&v1, raw, sizeof(v1));
    if (v1.magic != IMU_POWER_CONFIG_MAGIC || v1.version != IMU_POWER_CONFIG_VERSION_V1 ||
        !imu_power_config_valid_id(v1.config.device_id) ||
        !imu_power_config_valid_baud(v1.config.baudrate) ||
        imu_power_crc32(&v1, offsetof(imu_power_config_record_v1_t, crc32)) != v1.crc32) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->magic = v1.magic;
    out->version = IMU_POWER_CONFIG_VERSION;
    out->generation = v1.generation;
    imu_power_config_defaults(&out->config);
    out->config.device_id = v1.config.device_id;
    out->config.baudrate = v1.config.baudrate;
    out->config.feature_flags = v1.config.feature_flags;
    return true;
}

bool imu_power_config_valid_id(uint8_t id)
{
    return id <= 253u;
}

bool imu_power_config_valid_baud(uint32_t baudrate)
{
    return baudrate == 57600u || baudrate == 115200u || baudrate == 1000000u ||
           baudrate == 2000000u || baudrate == 3000000u;
}

bool imu_power_config_valid_gyro_fs(uint8_t code)
{
    return code <= 4u || code == 12u;
}

bool imu_power_config_valid_accel_fs(uint8_t code)
{
    return code <= 3u;
}

bool imu_power_config_valid_imu_odr(uint8_t code)
{
    return code >= 6u && code <= 8u;
}

bool imu_power_config_valid_sflp_odr(uint8_t code)
{
    return code >= 2u && code <= 5u;
}

bool imu_power_config_valid_rates(uint8_t imu_odr, uint8_t sflp_odr)
{
    if (!imu_power_config_valid_imu_odr(imu_odr) ||
        !imu_power_config_valid_sflp_odr(sflp_odr)) {
        return false;
    }
    /* imu_odr c is 120 << (c - 6) Hz; sflp_odr s is 15 << s Hz. */
    return (15u << sflp_odr) <= (120u << (imu_odr - 6u));
}

bool imu_power_config_valid(const imu_power_config_t *config)
{
    return config != NULL && imu_power_config_valid_id(config->device_id) &&
           imu_power_config_valid_baud(config->baudrate) &&
           imu_power_config_valid_gyro_fs(config->gyro_fs) &&
           imu_power_config_valid_accel_fs(config->accel_fs) &&
           imu_power_config_valid_rates(config->imu_odr, config->sflp_odr);
}

unsigned imu_power_config_gyro_ratio(uint8_t gyro_fs)
{
    switch (gyro_fs) {
    case 0u: return 1u;
    case 1u: return 2u;
    case 3u: return 8u;
    case 4u: return 16u;
    case 12u: return 32u;
    default: return 4u;
    }
}

void imu_power_config_defaults(imu_power_config_t *config)
{
    if (config != NULL) {
        memset(config, 0, sizeof(*config));
        config->device_id = (uint8_t)IMU_POWER_DEFAULT_ID;
        config->baudrate = IMU_POWER_DEFAULT_BAUD;
        config->gyro_fs = (uint8_t)IMU_POWER_DEFAULT_GYRO_FS;
        config->accel_fs = (uint8_t)IMU_POWER_DEFAULT_ACCEL_FS;
        config->imu_odr = (uint8_t)IMU_POWER_DEFAULT_IMU_ODR;
        config->sflp_odr = (uint8_t)IMU_POWER_DEFAULT_SFLP_ODR;
    }
}

uint32_t imu_power_crc32(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    unsigned bit;
    for (i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (bit = 0; bit < 8u; ++bit) {
            crc = (crc >> 1u) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

bool imu_power_config_load(const imu_power_config_storage_t *storage,
                           imu_power_config_t *config,
                           uint32_t *generation)
{
    imu_power_config_record_t records[IMU_POWER_CONFIG_SLOTS];
    bool valid[IMU_POWER_CONFIG_SLOTS] = {false, false};
    unsigned i;
    int selected = -1;

    if (config == NULL) {
        return false;
    }
    for (i = 0; i < IMU_POWER_CONFIG_SLOTS; ++i) {
        if (storage != NULL && storage->read != NULL &&
            storage->read(storage->context, i, &records[i], sizeof(records[i]))) {
            valid[i] = record_valid(&records[i]);
            if (!valid[i]) {
                imu_power_config_record_t upgraded;
                if (record_v1_upgrade(&records[i], &upgraded)) {
                    records[i] = upgraded;
                    valid[i] = true;
                }
            }
        }
    }
    for (i = 0; i < IMU_POWER_CONFIG_SLOTS; ++i) {
        if (valid[i] && (selected < 0 ||
            (uint32_t)(records[i].generation - records[selected].generation) < 0x80000000u)) {
            selected = (int)i;
        }
    }
    if (selected < 0) {
        imu_power_config_defaults(config);
        if (generation != NULL) {
            *generation = 0u;
        }
        return false;
    }
    *config = records[selected].config;
    if (generation != NULL) {
        *generation = records[selected].generation;
    }
    return true;
}

bool imu_power_config_save(const imu_power_config_storage_t *storage,
                           const imu_power_config_t *config,
                           uint32_t current_generation,
                           uint32_t *new_generation)
{
    imu_power_config_record_t record;
    unsigned slot;
    if (storage == NULL || storage->write == NULL || config == NULL ||
        !imu_power_config_valid(config)) {
        return false;
    }
    memset(&record, 0, sizeof(record));
    record.magic = IMU_POWER_CONFIG_MAGIC;
    record.version = IMU_POWER_CONFIG_VERSION;
    record.generation = current_generation + 1u;
    record.config = *config;
    record.crc32 = imu_power_crc32(&record, offsetof(imu_power_config_record_t, crc32));
    slot = record.generation & 1u;
    if (!storage->write(storage->context, slot, &record, sizeof(record))) {
        return false;
    }
    if (new_generation != NULL) {
        *new_generation = record.generation;
    }
    return true;
}
