#include "imu_power/imu_wire.h"

#include <string.h>

static void put_u16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8u);
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
        put_u16(&out[i * 2u], (uint16_t)sample->gyro_raw[i]);
        put_u16(&out[6u + i * 2u], sample->quat_half[i]);
    }
}

bool imu_power_quaternion_valid(const uint16_t quat_half[3])
{
    unsigned i;
    bool nonzero = false;
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
    }
    return nonzero;
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
