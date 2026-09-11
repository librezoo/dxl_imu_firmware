#ifndef IMU_POWER_CONFIG_H
#define IMU_POWER_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IMU_POWER_DEFAULT_ID 200u
#define IMU_POWER_DEFAULT_BAUD 1000000u
#define IMU_POWER_CONFIG_MAGIC 0x494D5532u
#define IMU_POWER_CONFIG_VERSION 1u
#define IMU_POWER_CONFIG_SLOTS 2u

typedef struct {
    uint8_t device_id;
    uint32_t baudrate;
    uint32_t feature_flags;
} imu_power_config_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t generation;
    imu_power_config_t config;
    uint32_t crc32;
} imu_power_config_record_t;

typedef struct {
    bool (*read)(void *context, unsigned slot, void *dst, size_t size);
    bool (*write)(void *context, unsigned slot, const void *src, size_t size);
    void *context;
} imu_power_config_storage_t;

bool imu_power_config_valid_id(uint8_t id);
bool imu_power_config_valid_baud(uint32_t baudrate);
void imu_power_config_defaults(imu_power_config_t *config);
bool imu_power_config_load(const imu_power_config_storage_t *storage,
                           imu_power_config_t *config,
                           uint32_t *generation);
bool imu_power_config_save(const imu_power_config_storage_t *storage,
                           const imu_power_config_t *config,
                           uint32_t current_generation,
                           uint32_t *new_generation);
uint32_t imu_power_crc32(const void *data, size_t size);

#endif

