# Microduck IMU/电源板固件

面向 CH32V203 的总线 IMU/电源板 C99 固件：Dynamixel Protocol 2.0、IMU 数据接口、配置保存和维护命令。编译所需 WCH SDK 源码已随仓库提交，克隆后不需要下载其它源码仓库、SDK 或子模块。硬件接口和构建细节见 [详细说明](docs/firmware-details.md)。

## 下载已验证固件

[firmware-20260917（诊断版本 0x0207）](https://github.com/cyber-zoo/microduck-imu-power-fw/releases/tag/firmware-20260917)
将 IMU 内部加速度、陀螺及 SFLP 融合的名义更新率提高到 240 Hz，保留已有的
总线收发修复、±500 dps 量程及芯片内置零偏修正。主控读取与 policy 仍保持 50 Hz。
在当前 CH32V203C8U6 + LSM6DSV16X 复刻板上完成了静态采样与持续合并读取验证。

- MounRiver Studio / WCH-LinkUtility：下载 `microduck-imu-power-fw-20260917.hex`。
- `wlink`：下载 `microduck-imu-power-fw-20260917.bin`，烧录基址为 `0x08000000`。
- `.elf`、`.map` 用于调试；`build-info.json`、`SHA256SUMS.txt` 沿用原发布的构建记录和校验文件。

```sh
wlink flash --address 0x08000000 ./microduck-imu-power-fw-20260917.bin
```

烧录前确认目标板并备份现有 Flash/配置。[发布说明](docs/releases/firmware-20260917.md)
记录具体修复、下载文件和验证范围；未进行机器人运动验收。

## 目录

- `src/`、`include/`：可移植协议与设备核心。
- `platform/`：CH32V203 平台实现。
- `tests/`：宿主机协议与配置测试。
- `vendor/wch-ch32v20x/`：固定版本 WCH SDK，含原始文件、来源说明和 SHA-256 清单。
- `build.ps1`：使用官方 IDE 自带编译器的 Windows 构建入口。
- `docs/`：详细说明与历史接口分析。

## 编译固件

先安装官方 [MounRiver Studio 2](https://www.mounriver.com/download)，确保安装了 WCH 的 **RISC-V Embedded GCC** 工具链。本项目已用其 `riscv-none-embed-gcc 8.2.0` 验证。

IDE 安装在 `C:\MounRiver\MounRiver_Studio2` 时，编译器完整路径为：

```text
C:\MounRiver\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Toolchain\RISC-V Embedded GCC\bin\riscv-none-embed-gcc.exe
```

同目录的 `riscv-none-embed-objcopy.exe` 和 `riscv-none-embed-size.exe` 用于生成镜像和查看大小。无需将这些工具加入 `PATH`。

在仓库根目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

若 IDE 安装在其它位置，指定它的安装根目录：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -MounRiverRoot 'D:\Tools\MounRiver_Studio2'
```

此入口只需 Windows PowerShell 5.1+ 与官方 IDE 自带工具链；无需单独安装 CMake、Make 或 Python。脚本先校验仓库内 SDK 的 SHA-256，再编译并生成：

- `build-ch32/imu_power_ch32.elf`：带符号的固件可执行文件。
- `build-ch32/imu_power_ch32.hex`、`build-ch32/imu_power_ch32.bin`：烧录镜像。
- `build-ch32/imu_power_ch32.map`：链接布局。

SDK 固定使用 `vendor/wch-ch32v20x/EVT/`，不需要配置 `WCH_EVT_ROOT`。已有 CMake 环境也可使用 [CMake 构建入口](docs/firmware-details.md#cmake-mcu-build-optional)。

## 宿主机测试（可选）

需要 CMake 3.16+ 和宿主机 C99 编译器，用于运行协议与配置测试。

```sh
cmake -S . -B build-host
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

构建产生的 HEX、BIN 与宿主机二进制放在 `build-*`，不进入源码提交。旧接口报告保留历史范围说明，不用于替代当前板卡设计。
