#include "imu_power/imu_wire.h"

#include <string.h>

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8u);
}

static float half_to_float(uint16_t value)
{
    unsigned exponent = (value >> 10u) & 0x1fu;
    unsigned fraction = value & 0x3ffu;
    float result;
    if (exponent == 0u) result = (float)fraction * 0x1p-24f;
    else {
        union { uint32_t u; float f; } bits;
        bits.u = ((uint32_t)(exponent + 112u) << 23u) | ((uint32_t)fraction << 13u);
        result = bits.f;
    }
    return (value & 0x8000u) ? -result : result;
}

static void put_u16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8u);
}

static int16_t gyro_output_count(const imu_power_sample_t *sample, unsigned axis)
{
    int32_t value = sample->gyro_raw[axis];
    if (sample->bias_valid) {
        /* The SFLP estimate is always 4.375 mdps/LSB. The wire gyro is
           17.5 mdps/LSB. Round only after subtraction, preserving real motion. */
        value = value * 4 - sample->gyro_bias_125[axis];
        value = (value + (value >= 0 ? 2 : -2)) / 4;
    }
    if (value > INT16_MAX) value = INT16_MAX;
    if (value < INT16_MIN) value = INT16_MIN;
    return (int16_t)value;
}

void imu_power_pack_sample(const imu_power_sample_t *sample,
                           uint8_t out[IMU_POWER_DATA_LENGTH])
{
    unsigned i;
    if (out == NULL) {
        return;
    }
    memset(out, 0, IMU_POWER_DATA_LENGTH);
    if (sample == NULL) {
        return;
    }
    for (i = 0; i < 3u; ++i) {
        put_u16(&out[i * 2u], (uint16_t)gyro_output_count(sample, i));
        put_u16(&out[6u + i * 2u], sample->quat_half[i]);
    }
}

bool imu_power_quaternion_valid(const uint16_t quat_half[3])
{
    unsigned i;
    bool nonzero = false;
    float norm_sq = 0.0f;
    if (quat_half == NULL) {
        return false;
    }
    for (i = 0; i < 3u; ++i) {
        uint16_t exponent = (uint16_t)((quat_half[i] >> 10u) & 0x1Fu);
        if (exponent == 0x1Fu) {
            return false;
        }
        if ((quat_half[i] & 0x7FFFu) != 0u) {
            nonzero = true;
        }
        {
            float value = half_to_float(quat_half[i]);
            norm_sq += value * value;
        }
    }
    /* Match robotd's finite/norm check and its all-zero startup sentinel. */
    return nonzero && norm_sq <= 1.02f;
}

void imu_power_sample_gyro(imu_power_sample_t *sample, imu_power_sample_clock_t *clock,
                           const uint8_t bytes[6], uint32_t now_ms)
{
    unsigned i;
    for (i = 0; i < 3u; ++i) sample->gyro_raw[i] = (int16_t)get_u16(&bytes[2u * i]);
    clock->gyro_ms = now_ms;
    clock->gyro_seen = true;
}

void imu_power_sample_fifo(imu_power_sample_t *sample, imu_power_sample_clock_t *clock,
                           const uint8_t fifo[7], uint32_t now_ms)
{
    unsigned i;
    switch (fifo[0] >> 3u) {
    case 0x13u: /* SFLP game rotation: IEEE binary16 x/y/z. */
        for (i = 0; i < 3u; ++i) sample->quat_half[i] = get_u16(&fifo[1u + 2u * i]);
        if (imu_power_quaternion_valid(sample->quat_half)) {
            clock->quat_ms = now_ms;
            clock->quat_seen = true;
            ++sample->sample_counter;
        } else {
            memset(sample->quat_half, 0, sizeof(sample->quat_half));
            clock->quat_seen = false;
        }
        break;
    case 0x16u: /* SFLP gyro bias: signed counts, 4.375 mdps/LSB. */
        for (i = 0; i < 3u; ++i) sample->gyro_bias_125[i] = (int16_t)get_u16(&fifo[1u + 2u * i]);
        sample->bias_valid = true;
        break;
    default:
        break;
    }
}

void imu_power_sample_age(imu_power_sample_t *sample, const imu_power_sample_clock_t *clock,
                          uint32_t now_ms)
{
    uint32_t gyro_age, quat_age, age_ms;
    sample->valid = clock->gyro_seen && clock->quat_seen;
    if (!sample->valid) {
        sample->age_us = UINT32_MAX;
        return;
    }
    gyro_age = now_ms - clock->gyro_ms;
    quat_age = now_ms - clock->quat_ms;
    age_ms = gyro_age > quat_age ? gyro_age : quat_age;
    sample->age_us = age_ms > UINT32_MAX / 1000u ? UINT32_MAX : age_ms * 1000u;
}

uint16_t imu_power_float_to_half(float value)
{
    union { float f; uint32_t u; } bits;
    uint32_t sign, mantissa;
    int32_t exponent;
    bits.f = value;
    sign = (bits.u >> 16u) & 0x8000u;
    exponent = (int32_t)((bits.u >> 23u) & 0xFFu) - 127 + 15;
    mantissa = bits.u & 0x7FFFFFu;
    if (exponent <= 0) {
        if (exponent < -10) {
            return (uint16_t)sign;
        }
        mantissa |= 0x800000u;
        return (uint16_t)(sign | ((mantissa >> (14 - exponent)) +
            ((mantissa >> (13 - exponent)) & 1u)));
    }
    if (exponent >= 31) {
        return (uint16_t)(sign | 0x7C00u | (mantissa != 0u ? 0x0200u : 0u));
    }
    return (uint16_t)(sign | ((uint32_t)exponent << 10u) |
                      ((mantissa + 0x1000u) >> 13u));
}
