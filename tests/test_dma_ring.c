#include "../platform/ch32v203_port.h"

#include <assert.h>
#include <stdio.h>

static unsigned consume_to_head(uint16_t tail, uint16_t remaining)
{
    uint16_t head = imu_power_ch32_rx_head(remaining);
    unsigned consumed = 0;
    /* Bound this model so a broken head cannot hang the test like the MCU. */
    while (tail != head && consumed < IMU_POWER_CH32_RX_SIZE) {
        tail = (uint16_t)((tail + 1u) % IMU_POWER_CH32_RX_SIZE);
        ++consumed;
    }
    assert(tail == head);
    return consumed;
}

int main(void)
{
    /* Twelve new bytes finish the ring; old bytes must not be replayed. */
    assert(consume_to_head(500u, 0u) == 12u);
    /* Zero and the reloaded count describe the same write position. */
    assert(consume_to_head(500u, 512u) == 12u);
    assert(consume_to_head(0u, 0u) == 0u);
    assert(consume_to_head(0u, 512u) == 0u);
    /* The next transfer crosses the wrap and stops at the new head. */
    assert(consume_to_head(500u, 508u) == 16u);
    assert(consume_to_head(4u, 508u) == 0u);
    puts("imu_power_dma_ring_tests: PASS");
    return 0;
}
