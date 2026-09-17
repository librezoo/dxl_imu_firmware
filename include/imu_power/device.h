#ifndef IMU_POWER_DEVICE_H
#define IMU_POWER_DEVICE_H

#include "config.h"
#include "dxl.h"
#include "imu_wire.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IMU_POWER_DIAG_ADDRESS 160u
#define IMU_POWER_IMU_CONFIG_ADDRESS 184u
#define IMU_POWER_CONFIG_ADDRESS 192u
#define IMU_POWER_GYRO_DIAG_ADDRESS 208u
#define IMU_POWER_MAX_READ 64u
#define IMU_POWER_CONFIG_CMD_SAVE 0x01u
#define IMU_POWER_CONFIG_CMD_REBOOT 0x02u
#define IMU_POWER_CONFIG_CMD_LOAD_DEFAULTS 0x03u
#define IMU_POWER_CONFIG_CMD_UNLOCK 0xA5u
#define IMU_POWER_CONFIG_CMD_UNLOCK_CONFIRM 0x5Au

#define IMU_POWER_STATUS_SAMPLE_VALID (1u << 0)
#define IMU_POWER_STATUS_IMU_READY (1u << 1)
#define IMU_POWER_STATUS_IMU_INIT_FAULT (1u << 2)
#define IMU_POWER_STATUS_IMU_FIFO_FAULT (1u << 3)
#define IMU_POWER_STATUS_SOFTBUS_RX_OVERFLOW (1u << 4)
#define IMU_POWER_STATUS_SOFTBUS_TX_OVERFLOW (1u << 5)
#define IMU_POWER_STATUS_SOFTBUS_ECHO_MISMATCH (1u << 6)

typedef struct {
    uint16_t firmware_version;
    uint8_t protocol_version;
    uint16_t status_flags;
    uint32_t sample_counter;
    uint32_t sample_age_us;
    uint16_t last_error;
    uint16_t reset_reason;
    uint16_t config_status;
    uint32_t error_count;
    /* Read back after IMU init: WHO_AM_I, CTRL1, CTRL2, CTRL3, CTRL6,
       CTRL8, embedded EMB_FUNC_EN_A, embedded SFLP_ODR. */
    uint8_t imu_registers[8];
} imu_power_diagnostics_t;

typedef struct {
    /* active_config is the target persisted configuration; runtime_* remain
       unchanged until reboot so an ID/baud write cannot strand the bus. */
    imu_power_config_t active_config;
    uint8_t runtime_id;
    uint32_t runtime_baudrate;
    imu_power_config_t staged_config;
    uint32_t config_generation;
    bool staged_valid;
    bool maintenance_unlocked;
    uint8_t unlock_step;
    uint8_t staged_command;
    bool staged_command_valid;
    bool reboot_requested;
    imu_power_sample_t sample;
    imu_power_diagnostics_t diagnostics;
    imu_power_config_storage_t storage;
} imu_power_device_t;

void imu_power_device_init(imu_power_device_t *device,
                           const imu_power_config_storage_t *storage);
void imu_power_device_set_sample(imu_power_device_t *device,
                                 const imu_power_sample_t *sample);
size_t imu_power_device_handle(imu_power_device_t *device,
                               const imu_power_dxl_frame_t *frame,
                               uint8_t *response, size_t response_capacity);

#endif
