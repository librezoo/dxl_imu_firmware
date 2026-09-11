#ifndef IMU_POWER_MAINTENANCE_H
#define IMU_POWER_MAINTENANCE_H

#include "device.h"

#include <stddef.h>

size_t imu_power_cli_execute(imu_power_device_t *device,
                             const char *line, char *out, size_t out_size);

#endif

