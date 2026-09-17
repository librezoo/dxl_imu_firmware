# Microduck IMU/电源板固件

面向 `CH32V203C8U6 + LSM6DSV16X` 的 MicroDuck 总线 IMU 固件。固件实现
Dynamixel Protocol 2.0、240 Hz SFLP/陀螺采样、持久化配置、诊断控制表，以及
DEBUG UART 到真实半双工总线的原始字节桥。

## 当前状态

| 项目 | 当前值 |
|---|---|
| 当前固件版本 | [`firmware-20260917-0x0208`](https://github.com/cyber-zoo/microduck-imu-power-fw/releases/tag/firmware-20260917-0x0208) |
| 上一发布版本 | [`firmware-20260917`](https://github.com/cyber-zoo/microduck-imu-power-fw/releases/tag/firmware-20260917)，诊断版本 `0x0207` |
| MCU / 晶振 | CH32V203C8U6 / 8 MHz |
| 内部 IMU | LSM6DSV16X，名义 240 Hz |
| DXL 默认配置 | Protocol 2.0，ID 200，1 Mbps |
| DEBUG 软总线 | PA9/PA10，固定 1 Mbps，8N1 |
| 最新默认 BIN | 14,324 字节，SHA-256 `b4f4f92a…586b0aa` |
| 宿主机测试 | 6/6 通过 |
| 实机长稳 | IMU 200 + 舵机 1/2，50 Hz，10 分钟，30,000/30,000 通过 |

完整实机证据见 [0x0208 DEBUG 软总线验证](docs/hardware-softbus-2026-09-17.md)。

## 快速开始

### 1. 编译固件

先安装官方 MounRiver Studio 2。仓库已经内置固定版本 WCH EVT SDK，无需下载
其它源码仓库、SDK 或子模块。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

默认从 `C:\MounRiver\MounRiver_Studio2` 使用 WCH RISC-V GCC 8.2.0。安装在
其它目录时：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 `
  -MounRiverRoot 'D:\Tools\MounRiver_Studio2'
```

标准产物位于 `build-ch32/`：

- `imu_power_ch32.elf`：带符号固件；
- `imu_power_ch32.hex`：WCH-LinkUtility / MounRiver 下载文件；
- `imu_power_ch32.bin`：原始烧录镜像；
- `imu_power_ch32.map`：链接布局。

应用区限制为 56 KiB，末尾两个 4 KiB Flash 页保留给双槽配置。

### 2. 烧录

MounRiver Studio 2 默认安装在 `C:\MounRiver\MounRiver_Studio2` 时，官方
WCH-LinkUtility 位于：

```text
C:\MounRiver\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Others\SWDTool\default\WCH-LinkUtility.exe
```

可从资源管理器直接打开，也可在 PowerShell 中启动：

```powershell
& 'C:\MounRiver\MounRiver_Studio2\resources\app\resources\win32\components\WCH\Others\SWDTool\default\WCH-LinkUtility.exe'
```

WCH-LinkUtility 使用 `build-ch32\imu_power_ch32.hex`。IDE 内置 OpenOCD 则位于：

```text
C:\MounRiver\MounRiver_Studio2\resources\app\resources\win32\components\WCH\OpenOCD\OpenOCD\bin\openocd.exe
```

`wlink` 是独立的第三方命令行工具，不随 MounRiver Studio 2 安装。已经另外安装
并加入 `PATH` 时，可以使用 BIN：

```powershell
wlink flash --address 0x08000000 .\build-ch32\imu_power_ch32.bin
```

烧录前确认目标为 CH32V203C8U6。解除读保护会整片擦除，包括末尾配置页；普通
应用区更新不得执行全片擦除。

### 3. 运行测试

```powershell
cmake -S . -B build-host
cmake --build build-host --config Release
ctest --test-dir build-host -C Release --output-on-failure

python tools\dxl_softbus_tool.py self-test
```

## DEBUG UART 原始软总线

默认构建将 USART1 映射为透明总线入口：

```text
PA9  DEBUG_TX  -> USB-UART RX
PA10 DEBUG_RX  <- USB-UART TX
GND            <-> USB-UART GND
baud           = 1,000,000, 8N1
```

- DEBUG RX 的字节突发按 UART IDLE 分帧，最大 512 字节，原样发送到 USART3
  真实 Dynamixel 总线。
- 真实总线流量持续进入 4,096 字节 DEBUG TX 队列。
- DEBUG 请求同时送入内部 DXL 解析器，因此可直接访问内部 IMU ID 200。
- 固件不修改、过滤或重新生成外部设备帧；错误 CRC 也会原样发送。
- 不提供多主仲裁、碰撞恢复或自动重试，调试端必须控制事务时序。

诊断状态字中的桥接位：

| 位 | 含义 |
|---:|---|
| 4 | DEBUG RX 溢出或突发被丢弃 |
| 5 | DEBUG TX 镜像队列溢出 |
| 6 | 请求物理回显不匹配 |

WCH-LinkE 自带串口最高只支持 921600，不能用于正式 1 Mbps 验收。实机测试使用
支持 1 Mbps 的 FTDI；50 Hz 长稳测试通过 D2XX 会话级 1 ms latency 完成。

## 舵机扫描与配置工具

[`tools/dxl_softbus_tool.py`](tools/dxl_softbus_tool.py) 将多波特率扫描、内部 IMU
维护配置和舵机 ID/波特率修改整合为一个 Python CLI。硬件命令依赖 `pyserial`：

```powershell
python -m pip install pyserial
```

全波特率、全 ID 扫描：

```powershell
python tools\dxl_softbus_tool.py scan --port COM3
```

限定扫描范围：

```powershell
python tools\dxl_softbus_tool.py scan --port COM3 `
  --bauds 57600 1000000 --ids 1-20,30
```

修改命令默认只输出计划，不写 EEPROM：

```powershell
python tools\dxl_softbus_tool.py configure --port COM3 `
  --current-id 1 --current-baud 57600 `
  --new-id 2 --new-baud 1000000
```

确认后显式添加 `--apply`：

```powershell
python tools\dxl_softbus_tool.py configure --port COM3 `
  --current-id 1 --current-baud 57600 `
  --new-id 2 --new-baud 1000000 --apply

# 只改波特率，ID 保持不变
python tools\dxl_softbus_tool.py configure --port COM3 `
  --current-id 1 --current-baud 57600 `
  --new-baud 1000000 --apply
```

工具会检查型号、当前 ID/波特率、目标 ID 冲突、扭矩和硬件错误；必要时关闭
扭矩，写后使用新配置读回验证，并在异常时恢复板内真实总线波特率。工具不会
自动重新使能扭矩或写入目标位置。

## 115200 文本 CLI 兼容构建

如需旧版文本维护口，而不是 1 Mbps 原始桥：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 `
  -BuildDirectory build-ch32-cli -DebugCli
```

CMake 对应选项为 `-DIMU_POWER_DEBUG_CLI=ON`。两个模式在同一固件中互斥，不使用
运行时转义切换。

## 验证摘要

- Protocol 2.0 CRC、字节填充、坏帧恢复、DMA 环形边界和 softbus 队列：宿主机
  6/6 测试通过。
- PowerShell 与 CMake MCU 构建生成相同 BIN。
- 正式 0x0208 镜像烧录后完整读回一致，内部 IMU ready，样本约 247.7 Hz。
- 两台 XL330-M288 已验证 ID 1/2、1 Mbps；安全控制表访问 142/142 通过。
- `SYNC_READ(124,12,[200,1,2])` 50 Hz、10 分钟、30,000/30,000 成功；平均
  延迟 2.89 ms，最大 6.70 ms，`last_error=0`。

尚未覆盖：真实断电持久化、IMU 物理断连恢复、看门狗强制复位、硬件异常帧注入
以及 115200 CLI 实机测试。

## 仓库结构

```text
include/imu_power/   可移植公共接口
src/                 协议、设备、配置、IMU 和 softbus 核心
platform/            CH32V203 外设适配
tests/               宿主机 C 测试
tools/               DEBUG 软总线 Python 工具
vendor/              固定版本 WCH 官方 SDK
docs/                架构、验证和发布记录
artifacts/            本地实机证据与可恢复临时归档（Git 忽略）
build-ch32/           标准 MCU 构建输出（Git 忽略）
build-host/           标准宿主机测试输出（Git 忽略）
```

文档导航见 [docs/README.md](docs/README.md)。第三方来源与许可证见
[THIRD_PARTY.md](THIRD_PARTY.md)、[provenance.json](provenance.json) 和
`vendor/wch-ch32v20x/manifest.json`。
