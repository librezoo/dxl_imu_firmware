#ifndef IMU_POWER_CH32V203_PORT_H
#define IMU_POWER_CH32V203_PORT_H

#include "imu_power/device.h"

#define IMU_POWER_CH32_RX_SIZE 512u

/* DMA's remaining count can be zero immediately before circular reload. */
static inline uint16_t imu_power_ch32_rx_head(uint16_t remaining)
{
    return (uint16_t)((IMU_POWER_CH32_RX_SIZE - remaining) % IMU_POWER_CH32_RX_SIZE);
}

void imu_power_ch32_port_init(imu_power_device_t *device);
void imu_power_ch32_port_poll(imu_power_device_t *device);
void imu_power_ch32_port_request_reboot(void);

#endif
