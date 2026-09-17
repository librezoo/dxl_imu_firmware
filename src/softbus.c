#include "imu_power/softbus.h"

#include <string.h>

void imu_power_softbus_init(imu_power_softbus_t *bridge)
{
    if (bridge != NULL) memset(bridge, 0, sizeof(*bridge));
}

bool imu_power_softbus_begin_injection(imu_power_softbus_t *bridge,
                                       const uint8_t *data, size_t length)
{
    if (bridge == NULL || data == NULL || length == 0u ||
        length > IMU_POWER_SOFTBUS_MAX_BURST ||
        bridge->echo_offset < bridge->echo_length) return false;
    memcpy(bridge->echo, data, length);
    bridge->echo_length = (uint16_t)length;
    bridge->echo_offset = 0u;
    return true;
}

bool imu_power_softbus_echo_pending(const imu_power_softbus_t *bridge)
{
    return bridge != NULL && bridge->echo_offset < bridge->echo_length;
}

void imu_power_softbus_end_injection(imu_power_softbus_t *bridge)
{
    if (bridge == NULL) return;
    bridge->echo_offset = 0u;
    bridge->echo_length = 0u;
}

static bool tx_push(imu_power_softbus_t *bridge, uint8_t byte)
{
    if (bridge->tx_count == IMU_POWER_SOFTBUS_TX_CAPACITY) {
        bridge->events |= IMU_POWER_SOFTBUS_EVENT_TX_OVERFLOW;
        return false;
    }
    bridge->tx[bridge->tx_head] = byte;
    bridge->tx_head = (uint16_t)((bridge->tx_head + 1u) %
                                 IMU_POWER_SOFTBUS_TX_CAPACITY);
    ++bridge->tx_count;
    return true;
}

bool imu_power_softbus_observe_bus_byte(imu_power_softbus_t *bridge,
                                        uint8_t byte, bool *suppressed)
{
    if (suppressed != NULL) *suppressed = false;
    if (bridge == NULL) return false;
    if (bridge->echo_offset < bridge->echo_length) {
        if (byte == bridge->echo[bridge->echo_offset]) {
            if (suppressed != NULL) *suppressed = true;
            ++bridge->echo_offset;
            if (bridge->echo_offset == bridge->echo_length) {
                bridge->echo_offset = 0u;
                bridge->echo_length = 0u;
            }
            return true;
        }
        bridge->events |= IMU_POWER_SOFTBUS_EVENT_ECHO_MISMATCH;
        bridge->echo_offset = 0u;
        bridge->echo_length = 0u;
    }
    return tx_push(bridge, byte);
}

bool imu_power_softbus_queue_debug(imu_power_softbus_t *bridge,
                                   const uint8_t *data, size_t length)
{
    size_t i;
    bool accepted = true;
    if (bridge == NULL || (length != 0u && data == NULL)) return false;
    for (i = 0u; i < length; ++i) {
        if (!tx_push(bridge, data[i])) accepted = false;
    }
    return accepted;
}

size_t imu_power_softbus_tx_peek(const imu_power_softbus_t *bridge,
                                 const uint8_t **data)
{
    size_t contiguous;
    if (data != NULL) *data = NULL;
    if (bridge == NULL || data == NULL || bridge->tx_count == 0u) return 0u;
    contiguous = IMU_POWER_SOFTBUS_TX_CAPACITY - bridge->tx_tail;
    if (contiguous > bridge->tx_count) contiguous = bridge->tx_count;
    *data = &bridge->tx[bridge->tx_tail];
    return contiguous;
}

void imu_power_softbus_tx_consume(imu_power_softbus_t *bridge, size_t length)
{
    if (bridge == NULL) return;
    if (length > bridge->tx_count) length = bridge->tx_count;
    bridge->tx_tail = (uint16_t)((bridge->tx_tail + length) %
                                 IMU_POWER_SOFTBUS_TX_CAPACITY);
    bridge->tx_count = (uint16_t)(bridge->tx_count - length);
}

size_t imu_power_softbus_tx_queued(const imu_power_softbus_t *bridge)
{
    return bridge == NULL ? 0u : bridge->tx_count;
}

uint16_t imu_power_softbus_take_events(imu_power_softbus_t *bridge)
{
    uint16_t events;
    if (bridge == NULL) return 0u;
    events = bridge->events;
    bridge->events = 0u;
    return events;
}
