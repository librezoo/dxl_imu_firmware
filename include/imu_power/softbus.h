#ifndef IMU_POWER_SOFTBUS_H
#define IMU_POWER_SOFTBUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IMU_POWER_SOFTBUS_MAX_BURST 512u
#define IMU_POWER_SOFTBUS_TX_CAPACITY 4096u

#define IMU_POWER_SOFTBUS_EVENT_TX_OVERFLOW 0x0001u
#define IMU_POWER_SOFTBUS_EVENT_ECHO_MISMATCH 0x0002u

typedef struct {
    uint8_t echo[IMU_POWER_SOFTBUS_MAX_BURST];
    uint16_t echo_length;
    uint16_t echo_offset;
    uint8_t tx[IMU_POWER_SOFTBUS_TX_CAPACITY];
    uint16_t tx_head;
    uint16_t tx_tail;
    uint16_t tx_count;
    uint16_t events;
} imu_power_softbus_t;

void imu_power_softbus_init(imu_power_softbus_t *bridge);

/* Record a controller-originated burst whose exact physical-bus echo must not
   be returned to the soft-bus controller. Only one echo can be pending. */
bool imu_power_softbus_begin_injection(imu_power_softbus_t *bridge,
                                       const uint8_t *data, size_t length);
bool imu_power_softbus_echo_pending(const imu_power_softbus_t *bridge);
void imu_power_softbus_end_injection(imu_power_softbus_t *bridge);

/* Observe one byte exactly as received by the real bus UART. Matching request
   echo bytes are suppressed; every other byte is queued for soft-bus TX. The
   optional suppressed result lets the local DXL parser avoid handling a
   physical request echo twice. */
bool imu_power_softbus_observe_bus_byte(imu_power_softbus_t *bridge,
                                        uint8_t byte, bool *suppressed);

/* Queue bytes generated locally (for example the internal IMU's physical
   status packet) for soft-bus TX when the receive path does not monitor TX. */
bool imu_power_softbus_queue_debug(imu_power_softbus_t *bridge,
                                   const uint8_t *data, size_t length);

/* Return the largest contiguous soft-bus TX span currently available. */
size_t imu_power_softbus_tx_peek(const imu_power_softbus_t *bridge,
                                 const uint8_t **data);
void imu_power_softbus_tx_consume(imu_power_softbus_t *bridge, size_t length);
size_t imu_power_softbus_tx_queued(const imu_power_softbus_t *bridge);

/* Read and clear sticky bridge events. */
uint16_t imu_power_softbus_take_events(imu_power_softbus_t *bridge);

#endif
