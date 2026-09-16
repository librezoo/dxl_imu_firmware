#include "imu_power/imu_wire.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    imu_power_sample_t sample = {0};
    imu_power_sample_clock_t clock = {0};
    const uint8_t gyro[] = {1,0,0xfe,0xff,3,0};
    const uint8_t quaternion[] = {0x13u << 3u,0,0x38,0,0,0,0x38};
    const uint8_t bias[] = {0x16u << 3u,4,0,0xf8,0xff,12,0};
    uint8_t out[IMU_POWER_DATA_LENGTH];
    unsigned i;

    imu_power_sample_age(&sample, &clock, 0u);
    assert(!sample.valid && sample.age_us == UINT32_MAX);
    imu_power_sample_gyro(&sample, &clock, gyro, 99u);
    assert(sample.gyro_raw[0] == 1 && sample.gyro_raw[1] == -2 && sample.gyro_raw[2] == 3);
    imu_power_sample_age(&sample, &clock, 100u);
    assert(!sample.valid && sample.sample_counter == 0u);

    imu_power_sample_fifo(&sample, &clock, quaternion, 100u);
    imu_power_sample_age(&sample, &clock, 100u);
    assert(sample.valid && sample.sample_counter == 1u && sample.age_us == 1000u);
    /* Polling without new sensor data must age the sample, not manufacture it. */
    for (i = 101u; i <= 110u; ++i) imu_power_sample_age(&sample, &clock, i);
    assert(sample.sample_counter == 1u && sample.age_us == 11000u);
    /* A fresh FIFO entry counts even if a stationary quaternion is identical. */
    imu_power_sample_fifo(&sample, &clock, quaternion, 112u);
    imu_power_sample_age(&sample, &clock, 112u);
    assert(sample.sample_counter == 2u && sample.age_us == 13000u);
    imu_power_sample_gyro(&sample, &clock, gyro, 113u);
    imu_power_sample_age(&sample, &clock, 113u);
    assert(sample.age_us == 1000u);
    imu_power_pack_sample(&sample, out);
    assert(memcmp(out, gyro, 6u) == 0); /* No estimate yet: retain the measurement. */

    imu_power_sample_fifo(&sample, &clock, bias, 113u);
    assert(sample.bias_valid && sample.gyro_bias_125[0] == 4 && sample.gyro_bias_125[1] == -8);
    assert(sample.sample_counter == 2u);
    imu_power_sample_fifo(&sample, &clock, (uint8_t[]){0xf8,1,2,3,4,5,6}, 114u);
    assert(sample.sample_counter == 2u && clock.quat_ms == 112u);
    imu_power_pack_sample(&sample, out);
    assert(memcmp(out, (uint8_t[]){0,0,0,0,0,0}, 6u) == 0);
    assert(memcmp(out + 6u, quaternion + 1u, 6u) == 0);
    /* Correction subtracts the estimate, not the measurement itself: real
       angular velocity survives, and the original readings remain intact. */
    imu_power_sample_gyro(&sample, &clock, (uint8_t[]){100,0,0x38,0xff,15,0}, 114u);
    imu_power_pack_sample(&sample, out);
    assert(memcmp(out, (uint8_t[]){99,0,0x3a,0xff,12,0}, 6u) == 0);
    assert(sample.gyro_raw[0] == 100 && sample.gyro_raw[1] == -200);
    sample.gyro_raw[0] = 1; sample.gyro_bias_125[0] = 6;
    sample.gyro_raw[1] = -1; sample.gyro_bias_125[1] = -6;
    imu_power_pack_sample(&sample, out);
    assert(out[0] == 0xffu && out[1] == 0xffu && out[2] == 1u && out[3] == 0u);
    sample.gyro_raw[0] = INT16_MAX; sample.gyro_bias_125[0] = INT16_MIN;
    sample.gyro_raw[1] = INT16_MIN; sample.gyro_bias_125[1] = INT16_MAX;
    imu_power_pack_sample(&sample, out);
    assert(out[0] == 0xffu && out[1] == 0x7fu && out[2] == 0u && out[3] == 0x80u);

    assert(!imu_power_quaternion_valid((uint16_t[]){0x3c00u,0,0x3c00u}));
    assert(!imu_power_quaternion_valid((uint16_t[]){0x7c01u,0,0}));
    assert(!imu_power_quaternion_valid((uint16_t[]){0,0,0}));
    assert(imu_power_quaternion_valid((uint16_t[]){0x3c00u,0x3000u,0}));
    assert(!imu_power_quaternion_valid((uint16_t[]){0x3c00u,0x3400u,0}));
    imu_power_sample_fifo(&sample, &clock, (uint8_t[]){0x98,0,0x3c,0,0,0,0x3c}, 115u);
    imu_power_sample_age(&sample, &clock, 115u);
    assert(!sample.valid && sample.age_us == UINT32_MAX && sample.sample_counter == 2u);
    assert(sample.quat_half[0] == 0u && sample.quat_half[2] == 0u);

    imu_power_sample_gyro(&sample, &clock, gyro, UINT32_MAX - 2u);
    imu_power_sample_fifo(&sample, &clock, quaternion, UINT32_MAX - 1u);
    imu_power_sample_age(&sample, &clock, 1u);
    assert(sample.valid && sample.age_us == 4000u);
    clock.gyro_ms = clock.quat_ms = 0u;
    imu_power_sample_age(&sample, &clock, UINT32_MAX / 1000u + 1u);
    assert(sample.age_us == UINT32_MAX);
    /* IEEE binary16 rounding, including carry and ties-to-even boundaries. */
    assert(imu_power_float_to_half(0.49999f) == 0x3800u);
    assert(imu_power_float_to_half(1.9999f) == 0x4000u);
    assert(imu_power_float_to_half(-1.9999f) == 0xc000u);
    assert(imu_power_float_to_half(0x1p-24f) == 1u);
    assert(imu_power_float_to_half(0x1p-25f) == 0u);
    assert(imu_power_float_to_half(0x1.8p-24f) == 2u);
    assert(imu_power_float_to_half(65504.0f) == 0x7bffu);
    assert(imu_power_float_to_half(65536.0f) == 0x7c00u);
    assert(imu_power_float_to_half(-0.0f) == 0x8000u);
    {
        union { uint32_t u; float f; } value;
        value.u = 0x7fc00001u;
        assert(imu_power_float_to_half(value.f) == 0x7e00u);
    }
    puts("imu_power_imu_sample_tests: PASS");
    return 0;
}
