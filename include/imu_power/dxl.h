#ifndef IMU_POWER_DXL_H
#define IMU_POWER_DXL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IMU_POWER_DXL_BROADCAST 0xFEu
#define IMU_POWER_DXL_MAX_PACKET 512u

typedef enum {
    IMU_POWER_DXL_PING = 0x01,
    IMU_POWER_DXL_READ = 0x02,
    IMU_POWER_DXL_WRITE = 0x03,
    IMU_POWER_DXL_REG_WRITE = 0x04,
    IMU_POWER_DXL_ACTION = 0x05,
    IMU_POWER_DXL_RESET = 0x06,
    IMU_POWER_DXL_STATUS = 0x55,
    IMU_POWER_DXL_SYNC_READ = 0x82,
    IMU_POWER_DXL_SYNC_WRITE = 0x83,
} imu_power_dxl_instruction_t;

typedef struct {
    uint8_t id;
    uint8_t instruction;
    const uint8_t *parameters;
    size_t parameter_length;
    bool broadcast;
} imu_power_dxl_frame_t;

typedef struct {
    uint8_t packet[IMU_POWER_DXL_MAX_PACKET];
    size_t length;
} imu_power_dxl_stream_t;

typedef enum {
    IMU_POWER_DXL_OK = 0,
    IMU_POWER_DXL_RESULT_FAIL = 1,
    IMU_POWER_DXL_INSTRUCTION = 2,
    IMU_POWER_DXL_CRC = 3,
    IMU_POWER_DXL_DATA_RANGE = 4,
    IMU_POWER_DXL_DATA_LENGTH = 5,
    IMU_POWER_DXL_ACCESS = 6,
} imu_power_dxl_error_t;

uint16_t imu_power_dxl_crc16(const uint8_t *data, size_t length);
bool imu_power_dxl_parse(const uint8_t *packet, size_t packet_length,
                         imu_power_dxl_frame_t *frame,
                         uint8_t *scratch, size_t scratch_size);
size_t imu_power_dxl_build_status(uint8_t id, uint8_t error,
                                  const uint8_t *payload, size_t payload_length,
                                  uint8_t *out, size_t out_capacity);

/* Feed wire bytes, consuming complete bad frames too. A new unescaped header
   resynchronizes a truncated frame. The returned frame points into scratch. */
bool imu_power_dxl_stream_feed(imu_power_dxl_stream_t *stream, uint8_t byte,
                               imu_power_dxl_frame_t *frame,
                               uint8_t *scratch, size_t scratch_size);

#endif
