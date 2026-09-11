#ifndef IMU_POWER_IMU_WIRE_H
#define IMU_POWER_IMU_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IMU_POWER_DATA_ADDRESS 124u
#define IMU_POWER_DATA_LENGTH 12u

typedef struct {
    int16_t gyro_raw[3];
    uint16_t quat_half[3];
    uint32_t sample_counter;
    uint32_t age_us;
    bool valid;
} imu_power_sample_t;

void imu_power_pack_sample(const imu_power_sample_t *sample,
                           uint8_t out[IMU_POWER_DATA_LENGTH]);
bool imu_power_quaternion_valid(const uint16_t quat_half[3]);
uint16_t imu_power_float_to_half(float value);

#endif

