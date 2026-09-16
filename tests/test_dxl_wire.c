#include "imu_power/dxl.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    /* ROBOTIS Protocol 2.0 manual: ID 1 PING and its example status. */
    static const uint8_t ping[] = {0xff,0xff,0xfd,0,1,3,0,1,0x19,0x4e};
    static const uint8_t ping_status[] = {
        0xff,0xff,0xfd,0,1,7,0,0x55,0,6,4,0x26,0x65,0x5d
    };
    /* Independent wire vector: status ID 20, payload FF FF FD 00.
       Length includes the inserted FD; CRC is over the stuffed packet. */
    static const uint8_t stuffed_status[] = {
        0xff,0xff,0xfd,0,20,9,0,0x55,0,0xff,0xff,0xfd,0xfd,0,0xce,0x7d
    };
    uint8_t scratch[512], out[512], corrupt[sizeof(stuffed_status)];
    imu_power_dxl_frame_t frame;
    size_t n;

    assert(imu_power_dxl_crc16(ping, sizeof(ping)-2u) == 0x4e19u);
    assert(imu_power_dxl_parse(ping, sizeof(ping), &frame, scratch, sizeof(scratch)));
    assert(frame.id == 1u && frame.instruction == 1u && frame.parameter_length == 0u);
    n = imu_power_dxl_build_status(1u, 0u, (uint8_t[]){6,4,0x26}, 3u, out, sizeof(out));
    assert(n == sizeof(ping_status) && memcmp(out, ping_status, n) == 0);

    assert(imu_power_dxl_parse(stuffed_status, sizeof(stuffed_status), &frame,
                                scratch, sizeof(scratch)));
    assert(frame.id == 20u && frame.instruction == 0x55u && frame.parameter_length == 5u);
    assert(memcmp(frame.parameters, (uint8_t[]){0,0xff,0xff,0xfd,0}, 5u) == 0);
    n = imu_power_dxl_build_status(20u, 0u, (uint8_t[]){0xff,0xff,0xfd,0}, 4u,
                                    out, sizeof(out));
    assert(n == sizeof(stuffed_status) && memcmp(out, stuffed_status, n) == 0);

    memcpy(corrupt, stuffed_status, sizeof(corrupt));
    corrupt[12] ^= 1u; /* Corruption of a stuffed byte must fail CRC. */
    assert(!imu_power_dxl_parse(corrupt, sizeof(corrupt), &frame, scratch, sizeof(scratch)));
    assert(!imu_power_dxl_parse(stuffed_status, sizeof(stuffed_status)-1u, &frame,
                                 scratch, sizeof(scratch)));
    assert(!imu_power_dxl_parse(stuffed_status, sizeof(stuffed_status), &frame, scratch, 2u));
    assert(imu_power_dxl_build_status(20u, 0u, (uint8_t[]){0xff,0xff,0xfd,0}, 4u,
                                      out, sizeof(stuffed_status)-1u) == 0u);

    /* A literal FD following an escaped header sequence is data, not stuffing. */
    {
        const uint8_t payload[] = {0xff,0xff,0xfd,0xfd,0xff,0xff,0xfd};
        n = imu_power_dxl_build_status(200u, 0u, payload, sizeof(payload), out, sizeof(out));
        assert(n == 11u + sizeof(payload) + 2u);
        assert(imu_power_dxl_parse(out, n, &frame, scratch, sizeof(scratch)));
        assert(frame.parameter_length == sizeof(payload)+1u);
        assert(memcmp(frame.parameters+1, payload, sizeof(payload)) == 0);
    }
    puts("imu_power_dxl_wire_tests: PASS");
    return 0;
}
