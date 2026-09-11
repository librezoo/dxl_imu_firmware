#ifndef IMU_POWER_CH32V203_PORT_H
#define IMU_POWER_CH32V203_PORT_H

#include "imu_power/device.h"

void imu_power_ch32_port_init(imu_power_device_t *device);
void imu_power_ch32_port_poll(imu_power_device_t *device);
void imu_power_ch32_port_request_reboot(void);

#endif
