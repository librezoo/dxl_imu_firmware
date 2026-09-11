# Microduck IMU/电源板固件

面向 CH32V203 的本地 C99 固件：Dynamixel Protocol 2.0、IMU 数据接口、配置保存和维护命令。硬件接口说明及已有构建记录见 `docs/firmware-details.md`。

## 目录

- `src/`、`include/`：可移植协议与设备核心。
- `platform/`：CH32V203 平台实现。
- `tests/`：宿主机协议与配置测试。
- `docs/`：详细说明与历史接口分析。

## 宿主机测试

需要 CMake 3.16+ 和 C99 编译器。

```sh
cmake -S . -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

## MCU 构建

WCH EVT SDK 为外部依赖，不复制到本仓库。通过 `WCH_EVT_ROOT` 环境变量或 `-DWCH_EVT_ROOT=/path/to/EVT` 指定 SDK，再按详细说明选择 RISC-V 工具链并设置 `-DIMU_POWER_BUILD_CH32=ON`。

构建产生的 HEX、BIN 与宿主机二进制放在 `build-*`，不进入源码提交。旧接口报告保留历史范围说明，不用于替代当前板卡设计。
