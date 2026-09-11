#include "ch32v203_port.h"

#ifdef IMU_POWER_CH32
#include "ch32v20x_misc.h"
#include "ch32v20x_iwdg.h"
int main(void)
{
    imu_power_device_t device;
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    imu_power_ch32_port_init(&device);
    while (1) {
        imu_power_ch32_port_poll(&device);
        IWDG_ReloadCounter();
    }
}
#else
int main(void) { return 0; }
#endif
