#include "imu_power/device.h"

#include <string.h>

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8u);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) |
           ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static void put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8u);
}

static void put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8u);
    p[2] = (uint8_t)(value >> 16u); p[3] = (uint8_t)(value >> 24u);
}

static uint16_t baud_enum(uint32_t baudrate)
{
    switch (baudrate) {
    case 57600u: return 0u;
    case 115200u: return 1u;
    case 1000000u: return 2u;
    case 2000000u: return 3u;
    case 3000000u: return 4u;
    default: return 0xFFFFu;
    }
}

static void diag_read(const imu_power_device_t *device, uint8_t *out)
{
    const imu_power_diagnostics_t *d = &device->diagnostics;
    put_u16(&out[0], d->firmware_version);
    out[2] = d->protocol_version;
    out[3] = device->runtime_id;
    put_u16(&out[4], baud_enum(device->runtime_baudrate));
    put_u16(&out[6], d->status_flags);
    put_u32(&out[8], d->sample_counter);
    put_u32(&out[12], d->sample_age_us);
    put_u16(&out[16], d->last_error);
    put_u16(&out[18], d->reset_reason);
    put_u32(&out[20], ((uint32_t)d->config_status << 16u) | device->config_generation);
}

static bool read_registers(const imu_power_device_t *device, uint16_t address,
                           uint16_t length, uint8_t *out)
{
    uint8_t sample[IMU_POWER_DATA_LENGTH];
    uint8_t diag[24];
    uint8_t config[7];
    uint8_t gyro_diag[14];
    uint16_t i;
    imu_power_pack_sample(&device->sample, sample);
    diag_read(device, diag);
    for (i = 0; i < 3u; ++i) {
        put_u16(&gyro_diag[2u * i], (uint16_t)device->sample.gyro_raw[i]);
        put_u16(&gyro_diag[6u + 2u * i], (uint16_t)device->sample.gyro_bias_125[i]);
    }
    gyro_diag[12] = device->sample.bias_valid ? 1u : 0u;
    gyro_diag[13] = device->sample.valid ? 1u : 0u;
    config[0] = device->staged_valid ? device->staged_config.device_id : device->active_config.device_id;
    put_u32(&config[1], device->staged_valid ? device->staged_config.baudrate : device->active_config.baudrate);
    config[5] = 0u;
    config[6] = (uint8_t)device->diagnostics.config_status;
    for (i = 0; i < length; ++i) {
        uint16_t a = (uint16_t)(address + i);
        if (a >= IMU_POWER_DATA_ADDRESS && a < IMU_POWER_DATA_ADDRESS + IMU_POWER_DATA_LENGTH) {
            out[i] = sample[a - IMU_POWER_DATA_ADDRESS];
        } else if (a >= IMU_POWER_DIAG_ADDRESS && a < IMU_POWER_DIAG_ADDRESS + sizeof(diag)) {
            out[i] = diag[a - IMU_POWER_DIAG_ADDRESS];
        } else if (a >= IMU_POWER_IMU_CONFIG_ADDRESS &&
                   a < IMU_POWER_IMU_CONFIG_ADDRESS + sizeof(device->diagnostics.imu_registers)) {
            out[i] = device->diagnostics.imu_registers[a - IMU_POWER_IMU_CONFIG_ADDRESS];
        } else if (a >= IMU_POWER_CONFIG_ADDRESS && a < IMU_POWER_CONFIG_ADDRESS + sizeof(config)) {
            out[i] = config[a - IMU_POWER_CONFIG_ADDRESS];
        } else if (a >= IMU_POWER_GYRO_DIAG_ADDRESS && a < IMU_POWER_GYRO_DIAG_ADDRESS + sizeof(gyro_diag)) {
            out[i] = gyro_diag[a - IMU_POWER_GYRO_DIAG_ADDRESS];
        } else if (a >= 136u && a < 160u) {
            out[i] = 0u;
        } else {
            return false;
        }
    }
    return true;
}

static bool maintenance_range(uint16_t address, uint16_t length)
{
    return length != 0u && address >= IMU_POWER_CONFIG_ADDRESS &&
           (uint32_t)address + length <= IMU_POWER_CONFIG_ADDRESS + 7u;
}

static bool apply_config_bytes(imu_power_config_t *config, uint16_t address,
                               const uint8_t *data, uint16_t length)
{
    uint32_t baud;
    if (!maintenance_range(address, length)) return false;
    if (address == 192u && length >= 1u) {
        if (!imu_power_config_valid_id(data[0])) return false;
        config->device_id = data[0];
        ++address; ++data; --length;
    }
    if (address <= 196u && address + length > 193u) {
        if (address != 193u || length < 4u) return false;
        baud = get_u32(data);
        if (!imu_power_config_valid_baud(baud)) return false;
        config->baudrate = baud;
    }
    return true;
}

static size_t status(imu_power_device_t *device, uint8_t error,
                     const uint8_t *payload, size_t length, uint8_t *response,
                     size_t capacity, bool broadcast)
{
    if (error != IMU_POWER_DXL_OK) {
        device->diagnostics.last_error = error;
        ++device->diagnostics.error_count;
    }
    if (broadcast) return 0u;
    return imu_power_dxl_build_status(device->runtime_id, error,
                                      payload, length, response, capacity);
}

void imu_power_device_init(imu_power_device_t *device,
                           const imu_power_config_storage_t *storage)
{
    uint32_t generation = 0u;
    memset(device, 0, sizeof(*device));
    if (storage != NULL) device->storage = *storage;
    if (!imu_power_config_load(&device->storage, &device->active_config, &generation)) {
        device->diagnostics.config_status = 1u;
    }
    device->config_generation = generation;
    device->runtime_id = device->active_config.device_id;
    device->runtime_baudrate = device->active_config.baudrate;
    device->diagnostics.firmware_version = 0x0209u;
    device->diagnostics.protocol_version = 2u;
    device->sample.valid = false;
}

void imu_power_device_set_sample(imu_power_device_t *device,
                                 const imu_power_sample_t *sample)
{
    if (device == NULL || sample == NULL) return;
    device->sample = *sample;
    device->diagnostics.sample_counter = sample->sample_counter;
    device->diagnostics.sample_age_us = sample->age_us;
    if (sample->valid) device->diagnostics.status_flags |= IMU_POWER_STATUS_SAMPLE_VALID;
    else device->diagnostics.status_flags &= (uint16_t)~IMU_POWER_STATUS_SAMPLE_VALID;
}

size_t imu_power_device_handle(imu_power_device_t *device,
                               const imu_power_dxl_frame_t *frame,
                               uint8_t *response, size_t response_capacity)
{
    uint16_t address, length;
    uint8_t payload[IMU_POWER_MAX_READ];
    bool addressed;
    if (device == NULL || frame == NULL || response == NULL) return 0u;
    /* Status packets belong to the controller, including a local TX echo. */
    if (frame->instruction == IMU_POWER_DXL_STATUS) return 0u;
    addressed = frame->broadcast || frame->id == device->runtime_id;
    if (!addressed && frame->instruction != IMU_POWER_DXL_SYNC_READ) return 0u;
    switch (frame->instruction) {
    case IMU_POWER_DXL_PING:
        return status(device, IMU_POWER_DXL_OK, NULL, 0u, response, response_capacity, frame->broadcast);
    case IMU_POWER_DXL_READ:
        if (frame->broadcast || frame->parameter_length != 4u) return 0u;
        address = get_u16(frame->parameters); length = get_u16(&frame->parameters[2]);
        if (length > IMU_POWER_MAX_READ || !read_registers(device, address, length, payload))
            return status(device, IMU_POWER_DXL_DATA_RANGE, NULL, 0u, response, response_capacity, false);
        return status(device, IMU_POWER_DXL_OK, payload, length, response, response_capacity, false);
    case IMU_POWER_DXL_SYNC_READ:
        if (!frame->broadcast || frame->parameter_length < 5u) return 0u;
        address = get_u16(frame->parameters); length = get_u16(&frame->parameters[2]);
        {
            size_t i;
            bool found = false;
            for (i = 4u; i < frame->parameter_length; ++i) {
                if (frame->parameters[i] == device->runtime_id) { found = true; break; }
            }
            if (!found || length > IMU_POWER_MAX_READ || !read_registers(device, address, length, payload))
                return 0u;
            return status(device, IMU_POWER_DXL_OK, payload, length, response, response_capacity, false);
        }
    case IMU_POWER_DXL_WRITE:
    case IMU_POWER_DXL_REG_WRITE:
        if (!addressed || frame->parameter_length < 2u) return 0u;
        address = get_u16(frame->parameters);
        length = (uint16_t)(frame->parameter_length - 2u);
        if (!maintenance_range(address, length))
            return status(device, IMU_POWER_DXL_ACCESS, NULL, 0u, response, response_capacity, frame->broadcast);
        if (address == 197u && length == 1u) {
            uint8_t command = frame->parameters[2];
            if (command == IMU_POWER_CONFIG_CMD_UNLOCK && device->unlock_step == 0u) {
                device->unlock_step = 1u;
                return status(device, IMU_POWER_DXL_OK, NULL, 0u, response, response_capacity, frame->broadcast);
            }
            if (command == IMU_POWER_CONFIG_CMD_UNLOCK_CONFIRM && device->unlock_step == 1u) {
                device->maintenance_unlocked = true; device->unlock_step = 0u;
                return status(device, IMU_POWER_DXL_OK, NULL, 0u, response, response_capacity, frame->broadcast);
            }
            if (!device->maintenance_unlocked)
                return status(device, IMU_POWER_DXL_ACCESS, NULL, 0u, response, response_capacity, frame->broadcast);
            if (frame->instruction == IMU_POWER_DXL_REG_WRITE) {
                device->staged_command = command;
                device->staged_command_valid = true;
            } else if (command == IMU_POWER_CONFIG_CMD_SAVE) {
                uint32_t generation;
                if (!imu_power_config_save(&device->storage, &device->active_config,
                                           device->config_generation, &generation))
                    return status(device, IMU_POWER_DXL_RESULT_FAIL, NULL, 0u, response, response_capacity, frame->broadcast);
                device->config_generation = generation;
                device->diagnostics.config_status = 0u;
            } else if (command == IMU_POWER_CONFIG_CMD_LOAD_DEFAULTS) {
                imu_power_config_defaults(&device->active_config);
                device->diagnostics.config_status = 2u;
            } else if (command == IMU_POWER_CONFIG_CMD_REBOOT) {
                device->reboot_requested = true;
            }
        } else {
            if (!device->maintenance_unlocked)
                return status(device, IMU_POWER_DXL_ACCESS, NULL, 0u, response, response_capacity, frame->broadcast);
            imu_power_config_t candidate = device->staged_valid ? device->staged_config : device->active_config;
            if (!apply_config_bytes(&candidate, address, &frame->parameters[2], length))
                return status(device, IMU_POWER_DXL_DATA_RANGE, NULL, 0u, response, response_capacity, frame->broadcast);
            device->staged_config = candidate;
            device->staged_valid = true;
            if (frame->instruction == IMU_POWER_DXL_WRITE) device->active_config = candidate;
        }
        return status(device, IMU_POWER_DXL_OK, NULL, 0u, response, response_capacity, frame->broadcast);
    case IMU_POWER_DXL_ACTION:
        if (!addressed) return 0u;
        if (device->staged_valid) { device->active_config = device->staged_config; device->staged_valid = false; }
        if (device->staged_command_valid) {
            if (device->staged_command == IMU_POWER_CONFIG_CMD_SAVE) {
                uint32_t generation;
                if (imu_power_config_save(&device->storage, &device->active_config,
                                          device->config_generation, &generation)) {
                    device->config_generation = generation;
                    device->diagnostics.config_status = 0u;
                } else {
                    device->diagnostics.last_error = IMU_POWER_DXL_RESULT_FAIL;
                }
            } else if (device->staged_command == IMU_POWER_CONFIG_CMD_LOAD_DEFAULTS) {
                imu_power_config_defaults(&device->active_config);
                device->diagnostics.config_status = 2u;
            } else if (device->staged_command == IMU_POWER_CONFIG_CMD_REBOOT) {
                device->reboot_requested = true;
            }
            device->staged_command_valid = false;
        }
        return status(device, IMU_POWER_DXL_OK, NULL, 0u, response, response_capacity, frame->broadcast);
    case IMU_POWER_DXL_RESET:
        if (!addressed) return 0u;
        device->reboot_requested = true;
        return status(device, IMU_POWER_DXL_OK, NULL, 0u, response, response_capacity, frame->broadcast);
    case IMU_POWER_DXL_SYNC_WRITE:
        if (!frame->broadcast || frame->parameter_length < 5u) return 0u;
        address = get_u16(frame->parameters); length = get_u16(&frame->parameters[2]);
        if (!maintenance_range(address, length)) return 0u;
        {
            size_t offset = 4u;
            while (offset + 1u + length <= frame->parameter_length) {
                if (frame->parameters[offset] == device->runtime_id && device->maintenance_unlocked) {
                    imu_power_config_t candidate = device->staged_valid ? device->staged_config : device->active_config;
                    if (apply_config_bytes(&candidate, address, &frame->parameters[offset + 1u], length)) {
                        device->active_config = candidate;
                        device->staged_config = candidate;
                        device->staged_valid = true;
                    }
                }
                offset += 1u + length;
            }
        }
        return 0u;
    default:
        if (addressed) return status(device, IMU_POWER_DXL_INSTRUCTION, NULL, 0u, response, response_capacity, frame->broadcast);
        return 0u;
    }
}
