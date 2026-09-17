#include "imu_power/softbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define assert(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #expression); \
        exit(1); \
    } \
} while (0)

static void expect_bytes(imu_power_softbus_t *bridge,
                         const uint8_t *expected, size_t length)
{
    size_t offset = 0u;
    while (offset < length) {
        const uint8_t *data;
        size_t available = imu_power_softbus_tx_peek(bridge, &data);
        assert(available != 0u && available <= length - offset);
        assert(memcmp(data, &expected[offset], available) == 0);
        imu_power_softbus_tx_consume(bridge, available);
        offset += available;
    }
    assert(imu_power_softbus_tx_queued(bridge) == 0u);
}

int main(void)
{
    imu_power_softbus_t bridge;
    uint8_t maximum[IMU_POWER_SOFTBUS_MAX_BURST];
    uint8_t too_large[IMU_POWER_SOFTBUS_MAX_BURST + 1u];
    uint8_t ambient[] = {0x12u, 0xFFu, 0x00u, 0x7Eu};
    uint8_t request[] = {0xFFu, 0xFFu, 0xFDu, 0u, 200u, 3u, 0u, 1u};
    uint8_t response[] = {0xFFu, 0xFFu, 0xFDu, 0u, 200u, 4u, 0u, 0x55u, 0u};
    size_t i;

    imu_power_softbus_init(&bridge);
    for (i = 0u; i < sizeof(ambient); ++i)
        assert(imu_power_softbus_observe_bus_byte(&bridge, ambient[i], NULL));
    expect_bytes(&bridge, ambient, sizeof(ambient));

    /* A DEBUG-originated request is still observed on the real bus, but its
       exact echo is not returned to DEBUG. The following device response is. */
    assert(imu_power_softbus_begin_injection(&bridge, request, sizeof(request)));
    assert(imu_power_softbus_echo_pending(&bridge));
    for (i = 0u; i < sizeof(request); ++i) {
        bool suppressed = false;
        assert(imu_power_softbus_observe_bus_byte(&bridge, request[i], &suppressed));
        assert(suppressed);
    }
    assert(!imu_power_softbus_echo_pending(&bridge));
    assert(imu_power_softbus_tx_queued(&bridge) == 0u);
    for (i = 0u; i < sizeof(response); ++i)
        assert(imu_power_softbus_observe_bus_byte(&bridge, response[i], NULL));
    expect_bytes(&bridge, response, sizeof(response));

    /* On a mismatch, retain every byte from the mismatch onward instead of
       swallowing the start of a real reply or collision. */
    assert(imu_power_softbus_begin_injection(&bridge, request, sizeof(request)));
    assert(imu_power_softbus_observe_bus_byte(&bridge, request[0], NULL));
    assert(imu_power_softbus_observe_bus_byte(&bridge, 0x42u, NULL));
    assert(imu_power_softbus_observe_bus_byte(&bridge, 0x43u, NULL));
    expect_bytes(&bridge, (uint8_t[]){0x42u, 0x43u}, 2u);
    assert((imu_power_softbus_take_events(&bridge) &
            IMU_POWER_SOFTBUS_EVENT_ECHO_MISMATCH) != 0u);

    memset(maximum, 0xA5, sizeof(maximum));
    memset(too_large, 0x5A, sizeof(too_large));
    assert(imu_power_softbus_begin_injection(&bridge, maximum, sizeof(maximum)));
    assert(!imu_power_softbus_begin_injection(&bridge, request, sizeof(request)));
    for (i = 0u; i < sizeof(maximum); ++i)
        assert(imu_power_softbus_observe_bus_byte(&bridge, maximum[i], NULL));
    assert(!imu_power_softbus_begin_injection(&bridge, too_large, sizeof(too_large)));

    assert(imu_power_softbus_begin_injection(&bridge, request, sizeof(request)));
    imu_power_softbus_end_injection(&bridge);
    assert(!imu_power_softbus_echo_pending(&bridge));
    assert(imu_power_softbus_queue_debug(&bridge, response, sizeof(response)));
    expect_bytes(&bridge, response, sizeof(response));

    /* Exercise wraparound and the full 4096-byte queue. */
    for (i = 0u; i < IMU_POWER_SOFTBUS_TX_CAPACITY; ++i)
        assert(imu_power_softbus_observe_bus_byte(&bridge, (uint8_t)i, NULL));
    assert(!imu_power_softbus_observe_bus_byte(&bridge, 0xEEu, NULL));
    assert((imu_power_softbus_take_events(&bridge) &
            IMU_POWER_SOFTBUS_EVENT_TX_OVERFLOW) != 0u);
    for (i = 0u; i < 3000u; ++i) {
        const uint8_t *data;
        assert(imu_power_softbus_tx_peek(&bridge, &data) != 0u);
        assert(*data == (uint8_t)i);
        imu_power_softbus_tx_consume(&bridge, 1u);
    }
    for (i = 0u; i < 3000u; ++i)
        assert(imu_power_softbus_observe_bus_byte(&bridge, (uint8_t)(i + 7u), NULL));
    assert(imu_power_softbus_tx_queued(&bridge) ==
           IMU_POWER_SOFTBUS_TX_CAPACITY - 3000u + 3000u);

    puts("imu_power_softbus_tests: PASS");
    return 0;
}
