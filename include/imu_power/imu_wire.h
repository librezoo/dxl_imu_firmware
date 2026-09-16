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
    /* SFLP bias uses fixed 125 dps units even when the gyro uses 500 dps. */
    int16_t gyro_bias_125[3];
    bool bias_valid;
} imu_power_sample_t;

typedef struct {
    uint32_t gyro_ms;
    uint32_t quat_ms;
    bool gyro_seen;
    bool quat_seen;
} imu_power_sample_clock_t;

void imu_power_sample_gyro(imu_power_sample_t *sample, imu_power_sample_clock_t *clock,
                           const uint8_t bytes[6], uint32_t now_ms);
void imu_power_sample_fifo(imu_power_sample_t *sample, imu_power_sample_clock_t *clock,
                           const uint8_t fifo[7], uint32_t now_ms);
void imu_power_sample_age(imu_power_sample_t *sample, const imu_power_sample_clock_t *clock,
                          uint32_t now_ms);

/* Wire gyro is in 500 dps counts with the on-chip SFLP bias removed when
   available. gyro_raw and gyro_bias_125 remain separately observable. */
void imu_power_pack_sample(const imu_power_sample_t *sample,
                           uint8_t out[IMU_POWER_DATA_LENGTH]);
bool imu_power_quaternion_valid(const uint16_t quat_half[3]);
uint16_t imu_power_float_to_half(float value);

#endif
