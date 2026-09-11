#include "imu_power/config.h"

#include <string.h>

static bool record_valid(const imu_power_config_record_t *record)
{
    if (record->magic != IMU_POWER_CONFIG_MAGIC ||
        record->version != IMU_POWER_CONFIG_VERSION ||
        !imu_power_config_valid_id(record->config.device_id) ||
        !imu_power_config_valid_baud(record->config.baudrate)) {
        return false;
    }
    return imu_power_crc32(record, offsetof(imu_power_config_record_t, crc32)) == record->crc32;
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

void imu_power_config_defaults(imu_power_config_t *config)
{
    if (config != NULL) {
        config->device_id = (uint8_t)IMU_POWER_DEFAULT_ID;
        config->baudrate = IMU_POWER_DEFAULT_BAUD;
        config->feature_flags = 0u;
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
        !imu_power_config_valid_id(config->device_id) ||
        !imu_power_config_valid_baud(config->baudrate)) {
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
