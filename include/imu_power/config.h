#ifndef IMU_POWER_CONFIG_H
#define IMU_POWER_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A board that must not share the bus with the standard ID 200 (for example a
   bench bridge) can be built with -DIMU_POWER_DEFAULT_ID=233u. */
#ifndef IMU_POWER_DEFAULT_ID
#define IMU_POWER_DEFAULT_ID 200u
#endif
/* Firmware version reported at address 160; override for test builds with -DIMU_POWER_FW_VERSION=0x....u */
#ifndef IMU_POWER_FW_VERSION
#define IMU_POWER_FW_VERSION 0x0210u
#endif
#define IMU_POWER_DEFAULT_BAUD 1000000u
#define IMU_POWER_CONFIG_MAGIC 0x494D5532u
#define IMU_POWER_CONFIG_VERSION 2u
#define IMU_POWER_CONFIG_VERSION_V1 1u
#define IMU_POWER_CONFIG_SLOTS 2u

/* IMU parameters hold the LSM6DSV16X register codes directly so the control
   table value, the stored value and the chip register always agree.
   gyro_fs  CTRL6.FS_G[3:0]   0:125 1:250 2:500 3:1000 4:2000 12:4000 dps
   accel_fs CTRL8.FS_XL[1:0]  0:2 1:4 2:8 3:16 g
   imu_odr  CTRL1_XL/CTRL2_G.ODR[3:0]  6:120 7:240 8:480 Hz
   sflp_odr SFLP_ODR.ODR[2:0] 2:60 3:120 4:240 5:480 Hz (must not exceed imu_odr) */
#define IMU_POWER_DEFAULT_GYRO_FS 2u
#define IMU_POWER_DEFAULT_ACCEL_FS 0u
#define IMU_POWER_DEFAULT_IMU_ODR 7u
#define IMU_POWER_DEFAULT_SFLP_ODR 4u

typedef struct {
    uint8_t device_id;
    uint8_t gyro_fs;
    uint8_t accel_fs;
    uint8_t imu_odr;
    uint32_t baudrate;
    uint32_t feature_flags;
    uint8_t sflp_odr;
    uint8_t reserved[3];
} imu_power_config_t;

/* Layout of the version 1 record, kept so a board upgraded in the field keeps
   its stored ID and baud rate. */
typedef struct {
    uint8_t device_id;
    uint32_t baudrate;
    uint32_t feature_flags;
} imu_power_config_v1_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t generation;
    imu_power_config_v1_t config;
    uint32_t crc32;
} imu_power_config_record_v1_t;

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
bool imu_power_config_valid_gyro_fs(uint8_t code);
bool imu_power_config_valid_accel_fs(uint8_t code);
bool imu_power_config_valid_imu_odr(uint8_t code);
bool imu_power_config_valid_sflp_odr(uint8_t code);
/* Cross-field check: SFLP cannot run faster than the sensors feeding it. */
bool imu_power_config_valid_rates(uint8_t imu_odr, uint8_t sflp_odr);
bool imu_power_config_valid(const imu_power_config_t *config);
/* Nominal gyro sensitivity as a multiple of 4.375 mdps/LSB (the 125 dps step). */
unsigned imu_power_config_gyro_ratio(uint8_t gyro_fs);
void imu_power_config_defaults(imu_power_config_t *config);
bool imu_power_config_load(const imu_power_config_storage_t *storage,
                           imu_power_config_t *config,
                           uint32_t *generation);
bool imu_power_config_save(const imu_power_config_storage_t *storage,
                           const imu_power_config_t *config,
                           uint32_t current_generation,
                           uint32_t *new_generation);
uint32_t imu_power_crc32(const void *data, size_t size);
/* Incremental form: start with 0xFFFFFFFF, feed the pieces, invert the final state. */
uint32_t imu_power_crc32_update(uint32_t state, const void *data, size_t size);

#endif

