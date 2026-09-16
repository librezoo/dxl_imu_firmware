#include "imu_power/device.h"
#include "imu_power/dxl.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Read ID 200 at the official IMU address and length. */
static const uint8_t read_imu[] = {
    0xff,0xff,0xfd,0,200,7,0,2,124,0,12,0,0x00,0x00
};

static unsigned feed(imu_power_dxl_stream_t *stream, const uint8_t *bytes, size_t n,
                     imu_power_dxl_frame_t *frame, uint8_t *scratch)
{
    unsigned parsed = 0;
    size_t i;
    for (i = 0; i < n; ++i)
        parsed += imu_power_dxl_stream_feed(stream, bytes[i], frame, scratch, 512u) ? 1u : 0u;
    return parsed;
}

int main(void)
{
    imu_power_dxl_stream_t stream = {{0}, 0};
    imu_power_dxl_frame_t frame;
    imu_power_device_t device;
    uint8_t scratch[512], request[sizeof(read_imu)], bad[sizeof(read_imu)], response[512];
    uint16_t crc;
    unsigned i;
    memcpy(request, read_imu, sizeof(request));
    crc = imu_power_dxl_crc16(request, sizeof(request)-2u);
    request[sizeof(request)-2u] = (uint8_t)crc;
    request[sizeof(request)-1u] = (uint8_t)(crc >> 8u);

    assert(feed(&stream, request, sizeof(request), &frame, scratch) == 1u);
    assert(frame.id == 200u && frame.instruction == IMU_POWER_DXL_READ);
    memcpy(bad, request, sizeof(bad));
    bad[sizeof(bad)-1u] ^= 1u;
    assert(feed(&stream, bad, sizeof(bad), &frame, scratch) == 0u);
    assert(stream.length == 0u);
    /* No idle period is needed to recover from a corrupt frame. */
    for (i = 0; i < 50u; ++i)
        assert(feed(&stream, request, sizeof(request), &frame, scratch) == 1u);

    /* A truncated packet or plausible but wrong length cannot swallow the
       following request. No 30 ms idle timeout occurs on the real 50 Hz bus. */
    for (i = 1; i < sizeof(request); ++i) {
        assert(feed(&stream, request, i, &frame, scratch) == 0u);
        assert(feed(&stream, request, sizeof(request), &frame, scratch) == 1u);
    }
    memcpy(bad, request, sizeof(bad)); bad[5] = 200u;
    assert(feed(&stream, bad, sizeof(bad), &frame, scratch) == 0u);
    assert(feed(&stream, request, sizeof(request), &frame, scratch) == 1u);
    memcpy(bad, request, sizeof(bad)); bad[5] = 0xffu; bad[6] = 0xffu;
    assert(feed(&stream, bad, sizeof(bad), &frame, scratch) == 0u);
    assert(feed(&stream, request, sizeof(request), &frame, scratch) == 1u);
    assert(feed(&stream, (uint8_t[]){0x12,0xff,0xff,0xff}, 4u, &frame, scratch) == 0u);
    assert(feed(&stream, request, sizeof(request), &frame, scratch) == 1u);

    {
        /* Captured official combined SYNC_READ request, with independent CRC. */
        const uint8_t combined[] = {
            0xff,0xff,0xfd,0,0xfe,0x17,0,0x82,0x7c,0,0x0c,0,
            200,20,21,22,23,24,30,31,32,33,34,10,11,12,13,14,0xd6,0xdd
        };
        assert(feed(&stream, combined, sizeof(combined), &frame, scratch) == 1u);
        assert(frame.broadcast && frame.instruction == IMU_POWER_DXL_SYNC_READ);
        assert(frame.parameter_length == 20u && frame.parameters[4] == 200u);
        imu_power_device_init(&device, NULL);
        assert(imu_power_device_handle(&device, &frame, response, sizeof(response)) == 23u);
    }

    {
        /* The apparent header straddles payload and CRC. CRC is not stuffed;
           accept this valid packet before attempting header resynchronization. */
        const uint8_t crc_header[] = {
            0xff,0xff,0xfd,0,200,8,0,0x55,0,0x82,0xeb,0xff,0xff,0xfd,0
        };
        assert(feed(&stream, crc_header, sizeof(crc_header), &frame, scratch) == 1u);
        assert(frame.parameter_length == 5u && frame.parameters[1] == 0x82u);
        assert(feed(&stream, request, sizeof(request), &frame, scratch) == 1u);
    }

    /* Other devices' stuffed status packets must be consumed, not poison
       the stream. An echoed status using our ID must never elicit a reply. */
    {
        const uint8_t stuffed[] = {
            0xff,0xff,0xfd,0,20,9,0,0x55,0,0xff,0xff,0xfd,0xfd,0,0xce,0x7d
        };
        size_t n;
        imu_power_device_init(&device, NULL);
        assert(feed(&stream, stuffed, sizeof(stuffed), &frame, scratch) == 1u);
        assert(imu_power_device_handle(&device, &frame, response, sizeof(response)) == 0u);
        n = imu_power_dxl_build_status(200u, 0u, NULL, 0u, response, sizeof(response));
        assert(feed(&stream, response, n, &frame, scratch) == 1u);
        assert(imu_power_device_handle(&device, &frame, response, sizeof(response)) == 0u);
        assert(device.diagnostics.error_count == 0u);
        assert(feed(&stream, request, sizeof(request), &frame, scratch) == 1u);
        assert(imu_power_device_handle(&device, &frame, response, sizeof(response)) != 0u);
    }
    puts("imu_power_dxl_stream_tests: PASS");
    return 0;
}
