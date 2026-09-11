#include "imu_power/maintenance.h"

static size_t text_len(const char *text)
{
    size_t n = 0u;
    while (text[n] != '\0') ++n;
    return n;
}

static size_t append_text(char *out, size_t capacity, size_t used, const char *text)
{
    size_t n = text_len(text);
    size_t i;
    if (used >= capacity) return used;
    if (n > capacity - used) n = capacity - used;
    for (i = 0u; i < n; ++i) out[used + i] = text[i];
    return used + n;
}

static size_t append_u32(char *out, size_t capacity, size_t used, uint32_t value)
{
    char digits[10];
    size_t n = 0u;
    if (value == 0u) return append_text(out, capacity, used, "0");
    while (value != 0u && n < sizeof(digits)) {
        digits[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (n != 0u) {
        char digit = digits[--n];
        if (used < capacity) out[used++] = digit;
    }
    return used;
}

static size_t append_i16(char *out, size_t capacity, size_t used, int16_t value)
{
    if (value < 0) {
        used = append_text(out, capacity, used, "-");
        return append_u32(out, capacity, used, (uint32_t)(-(int32_t)value));
    }
    return append_u32(out, capacity, used, (uint32_t)value);
}

static size_t finish(char *out, size_t capacity, size_t used)
{
    out[used < capacity ? used : capacity - 1u] = '\0';
    return used;
}

static bool equals(const char *left, const char *right)
{
    size_t i = 0u;
    while (left[i] != '\0' && left[i] == right[i]) ++i;
    return left[i] == '\0' && right[i] == '\0';
}

static bool prefix_value(const char *line, const char *prefix, uint32_t *value)
{
    size_t i = 0u;
    uint32_t parsed = 0u;
    bool any = false;
    while (prefix[i] != '\0') {
        if (line[i] != prefix[i]) return false;
        ++i;
    }
    while (line[i] == ' ') ++i;
    while (line[i] >= '0' && line[i] <= '9') {
        parsed = parsed * 10u + (uint32_t)(line[i] - '0');
        any = true;
        ++i;
    }
    if (!any || line[i] != '\0') return false;
    *value = parsed;
    return true;
}

size_t imu_power_cli_execute(imu_power_device_t *device,
                             const char *line, char *out, size_t out_size)
{
    uint32_t value;
    uint32_t generation;
    size_t used = 0u;
    if (device == NULL || line == NULL || out == NULL || out_size == 0u) return 0u;
    if (equals(line, "get")) {
        used = append_text(out, out_size, used, "id=");
        used = append_u32(out, out_size, used, device->active_config.device_id);
        used = append_text(out, out_size, used, " baud=");
        used = append_u32(out, out_size, used, device->active_config.baudrate);
        used = append_text(out, out_size, used, " generation=");
        used = append_u32(out, out_size, used, device->config_generation);
    } else if (equals(line, "diag")) {
        used = append_text(out, out_size, used, "status=");
        used = append_u32(out, out_size, used, device->diagnostics.status_flags);
        used = append_text(out, out_size, used, " samples=");
        used = append_u32(out, out_size, used, device->diagnostics.sample_counter);
        used = append_text(out, out_size, used, " age_us=");
        used = append_u32(out, out_size, used, device->diagnostics.sample_age_us);
        used = append_text(out, out_size, used, " errors=");
        used = append_u32(out, out_size, used, device->diagnostics.error_count);
    } else if (prefix_value(line, "set id", &value) && value <= 253u) {
        device->active_config.device_id = (uint8_t)value;
        device->staged_config = device->active_config;
        device->staged_valid = true;
        used = append_text(out, out_size, used, "ok");
    } else if (prefix_value(line, "set baud", &value) && imu_power_config_valid_baud(value)) {
        device->active_config.baudrate = value;
        device->staged_config = device->active_config;
        device->staged_valid = true;
        used = append_text(out, out_size, used, "ok");
    } else if (equals(line, "save")) {
        if (imu_power_config_save(&device->storage, &device->active_config,
                                  device->config_generation, &generation)) {
            device->config_generation = generation;
            device->diagnostics.config_status = 0u;
            used = append_text(out, out_size, used, "ok");
        } else {
            used = append_text(out, out_size, used, "error save");
        }
    } else if (equals(line, "load-defaults")) {
        imu_power_config_defaults(&device->active_config);
        device->staged_config = device->active_config;
        device->staged_valid = true;
        device->diagnostics.config_status = 2u;
        used = append_text(out, out_size, used, "ok");
    } else if (equals(line, "reboot")) {
        device->reboot_requested = true;
        used = append_text(out, out_size, used, "ok");
    } else if (equals(line, "dump-imu")) {
        used = append_text(out, out_size, used, "gyro=");
        used = append_i16(out, out_size, used, device->sample.gyro_raw[0]);
        used = append_text(out, out_size, used, ",");
        used = append_i16(out, out_size, used, device->sample.gyro_raw[1]);
        used = append_text(out, out_size, used, ",");
        used = append_i16(out, out_size, used, device->sample.gyro_raw[2]);
        used = append_text(out, out_size, used, " valid=");
        used = append_u32(out, out_size, used, device->sample.valid ? 1u : 0u);
    } else if (equals(line, "dump-errors")) {
        used = append_text(out, out_size, used, "last=");
        used = append_u32(out, out_size, used, device->diagnostics.last_error);
        used = append_text(out, out_size, used, " count=");
        used = append_u32(out, out_size, used, device->diagnostics.error_count);
        used = append_text(out, out_size, used, " reset=");
        used = append_u32(out, out_size, used, device->diagnostics.reset_reason);
    } else {
        used = append_text(out, out_size, used, "error command");
    }
    used = append_text(out, out_size, used, "\r\n");
    return finish(out, out_size, used);
}
