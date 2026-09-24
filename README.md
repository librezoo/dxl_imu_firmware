# Microduck IMU/电源板固件

面向 `CH32V203C8U6 + LSM6DSV16X` 的 MicroDuck 总线 IMU 固件。固件实现
Dynamixel Protocol 2.0、240 Hz SFLP/陀螺采样、持久化配置、诊断控制表，以及
USART2 到真实半双工总线的原始字节桥；USART1 同时保留文本调试口。

## 当前状态

| 项目 | 当前值 |
|---|---|
| 当前固件版本 | [`firmware-20260924-0x020a`](https://github.com/cyber-zoo/microduck-imu-power-fw/releases/tag/firmware-20260924-0x020a) |
| 上一发布版本 | [`firmware-20260918-0x0209`](https://github.com/cyber-zoo/microduck-imu-power-fw/releases/tag/firmware-20260918-0x0209) |
| MCU / 晶振 | CH32V203C8U6 / 8 MHz |
| 内部 IMU | LSM6DSV16X，名义 240 Hz |
| DXL 默认配置 | Protocol 2.0，ID 200，1 Mbps |
| 原始软总线 | USART2 PA2/PA3，固定 1 Mbps，8N1 |
| 文本调试口 | USART1 PA9/PA10，固定 115200，8N1 |
| 当前发布 BIN | 15,956 字节，SHA-256 `3d132032…023a51e` |
| 宿主机测试 | 6/6 通过 |
| 0x0208 实机记录 | IMU 200 + 舵机 1/2，50 Hz，10 分钟，30,000/30,000 通过；舵机 3 四设备冒烟 100/100 通过 |
| 0x0209 修复验证 | IMU + 15 舵机，50 Hz，30,001/30,001；单 IMU 200 Hz 与混合读取各 12,000/12,000，均零错误 |
| 0x020A 本地验证 | USART2 内部 IMU READ 300/300、SYNC_READ 100/100；USART1 CLI 与外部 ID 3 转发通过 |

0x020A 的 USART2 迁移、烧录读回和串口验证见 [累计验证记录](docs/validation.md)。
0x0209 的原因定位和长稳验证见
[DEBUG RX 空闲电平修复](docs/debug-rx-idle-fix-2026-09-18.md)；更早的软总线记录见
[0x0208 DEBUG 软总线验证](docs/hardware-softbus-2026-09-17.md)。

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

## USART2 原始软总线与 USART1 调试口

默认构建同时启用两个独立接口：

```text
PA2  SOFTBUS_TX -> 软总线 USB-UART RX
PA3  SOFTBUS_RX <- 软总线 USB-UART TX
GND             <-> USB-UART GND
softbus baud    = 1,000,000, 8N1

PA9  DEBUG_TX   -> 调试 USB-UART RX
PA10 DEBUG_RX   <- 调试 USB-UART TX
debug baud      = 115,200, 8N1
```

- USART2 RX 的字节突发按 UART IDLE 分帧，最大 512 字节，原样发送到 USART3
  真实 Dynamixel 总线。
- 真实总线流量持续进入 4,096 字节 USART2 TX 队列。
- 软总线请求同时送入内部 DXL 解析器，因此可直接访问内部 IMU ID 200。
- USART1 文本 CLI 与软总线同时工作，无需选择不同固件。
- 固件不修改、过滤或重新生成外部设备帧；错误 CRC 也会原样发送。
- 不提供多主仲裁、碰撞恢复或自动重试，调试端必须控制事务时序。

诊断状态字中的桥接位：

| 位 | 含义 |
|---:|---|
| 4 | USART2 软总线 RX 溢出或突发被丢弃 |
| 5 | USART2 软总线 TX 镜像队列溢出 |
| 6 | 请求物理回显不匹配 |

WCH-LinkE 自带串口最高只支持 921600，不能用于正式 1 Mbps 验收。实机测试使用
支持 1 Mbps 的 FTDI；50 Hz 长稳测试通过 D2XX 会话级 1 ms latency 完成。

## DYNAMIXEL Wizard 2.0 图形上位机

舵机配置、单机动作测试、实时曲线和手工数据包调试，推荐使用 ROBOTIS 官方
DYNAMIXEL Wizard 2.0。它支持 Windows 10/11 64 位、Dynamixel Protocol 2.0 和
XL330-M288。

- [ROBOTIS 官方下载与使用说明](https://emanual.robotis.com/docs/en/software/dynamixel/dynamixel_wizard2/#software-download)

先在 Windows 设备管理器的“端口（COM 和 LPT）”中确认连接 PA2/PA3 的
软总线 USB-UART 实际端口号。使用前关闭 Python 测试器和其它占用该端口的程序，
然后在 Wizard 中配置：

```text
Tools -> Options -> Scan
Port:       实际软总线 USB-UART 端口（例如 COM5）
Baudrate:   1,000,000
Protocol:   2.0
ID range:   1-3
```

执行 `Device -> Scan` 后，Wizard 会列出当前实际接入并响应的设备；不要假定设备
数量或 ID。选择设备即可读取和修改控制表。手动运动测试建议依次设置：

```text
Torque Enable(64)       = 0
Profile Acceleration(108) = 20
Profile Velocity(112)     = 80
Goal Position(116)        = 当前 Present Position(132)
Torque Enable(64)         = 1
```

随后只小幅修改 Goal Position，并在结束或异常时将 Torque Enable 恢复为 0。测试前
应让机构悬空、确认运动范围无遮挡并准备断开舵机电源。

内部 IMU ID 200 是自定义设备，Wizard 可能不会把它识别为标准舵机。可在
`View -> Packet` 中手动发送 `READ(124,12)`，或使用下节 Python 工具访问。不要
通过 Wizard 对内部 IMU执行舵机 Firmware Update 或 Factory Reset。Wizard 适合
配置和低频调试，不替代机器人实时 50 Hz 控制程序。

## IMU、舵机检查与扫描配置工具

[`tools/dxl_softbus_tool.py`](tools/dxl_softbus_tool.py) 将多波特率扫描、内部 IMU
采样诊断、XL330 状态检查和舵机 ID/波特率修改整合为一个 Python CLI。硬件命令
依赖 `pyserial`：

```powershell
python -m pip install pyserial
```

以下 PowerShell 示例先保存实际端口；请把 `COMx` 替换为设备管理器显示的端口号：

```powershell
$softbusPort = 'COMx'
```

一次检查板内 IMU、扫描当前真实总线波特率，并读取找到的 XL330 电压、温度、
扭矩和硬件错误：

```powershell
python tools\dxl_softbus_tool.py check --port $softbusPort `
  --ids 0-253 --samples 20 `
  --json-out artifacts\combined-check.json
```

添加 `--show-samples` 可逐条显示陀螺仪角速度和四元数。`check` 默认只扫描板内
当前总线波特率；显式传入 `--bauds 57600 115200 1000000 2000000 3000000`
可检查全部支持的速率。状态包中的 `0x80` Alert 会被报告为警告，不会再导致
扫描中止；低 7 位协议错误仍会失败。默认未找到外部设备属于 FAIL；仅检查 IMU
时添加 `--allow-no-external`，将其降级为 WARN。

全部子命令使用统一结果分级：

| 状态 | 退出码 | 含义 |
|---|---:|---|
| `PASS` | 0 | 所有要求均通过 |
| `WARN` | 1 | 命令完成，但存在 Alert、粘滞诊断位或允许的空扫描等警告 |
| `FAIL` | 2 | 检查失败、设备状态危险或命令发生异常 |

交互终端默认显示绿/黄/红状态和最终大标题；重定向输出或设置 `NO_COLOR` 时自动
使用纯文本。可用 `--color auto|always|never` 显式控制。最后一行始终不带颜色，
可供 PowerShell 或 CI 解析：

```text
==================== FINAL RESULT: FAIL ====================
RESULT command=check status=FAIL pass=5 warn=1 fail=3 external=1
```

常用结果字段：

| 输出 | 含义 |
|---|---|
| `IMU ... status=0x0003` | IMU 样本有效且初始化完成 |
| `IMU_SAMPLES ... quaternion_valid=N` | N 个四元数样本通过有限值与范数检查 |
| `[PASS] DEVICE_FOUND` | 找到一个无 Alert 的真实总线设备 |
| `[WARN] DEVICE_ALERT` | 找到设备，但其状态包带 `0x80` Alert |
| `alert=True` / `status_error=0x80` | 设备正常回复，但同时报告硬件警告 |
| `TOTAL_EXTERNAL_RESPONDERS` | 扫描到的外部设备总数；板内 ID 200 不计入 |
| `FINAL RESULT` / `RESULT` | 最终人工可读状态及机器可读统计 |

诊断状态位 4–6 为粘滞记录，出现 `softbus_rx_overflow` 等提示时代表启动以来曾经
发生过对应事件；可在复位后重新运行 `check`，判断它是否持续复现。执行串口测试
时不要让 WCH-Link 保持调试附着，否则调试器可能临时改变目标时钟并干扰 UART。
`check --json-out` 保留 IMU、外部设备和 `warnings` 字段，并增加结构化 `findings`
及 `result`，便于程序按稳定检查码和严重度处理。

PowerShell 可直接检查退出码：

```powershell
python tools\dxl_softbus_tool.py check --port $softbusPort --ids 0-253
switch ($LASTEXITCODE) {
  0 { 'PASS' }
  1 { 'WARN：请查看黄色诊断项' }
  2 { 'FAIL：请查看红色失败项' }
}
```

全波特率、全 ID 扫描：

```powershell
python tools\dxl_softbus_tool.py scan --port $softbusPort
```

限定扫描范围：

```powershell
python tools\dxl_softbus_tool.py scan --port $softbusPort `
  --bauds 57600 1000000 --ids 1-20,30
```

修改命令默认只输出计划，不写 EEPROM：

```powershell
python tools\dxl_softbus_tool.py configure --port $softbusPort `
  --current-id 1 --current-baud 57600 `
  --new-id 2 --new-baud 1000000
```

确认后显式添加 `--apply`：

```powershell
python tools\dxl_softbus_tool.py configure --port $softbusPort `
  --current-id 1 --current-baud 57600 `
  --new-id 2 --new-baud 1000000 --apply

# 只改波特率，ID 保持不变
python tools\dxl_softbus_tool.py configure --port $softbusPort `
  --current-id 1 --current-baud 57600 `
  --new-baud 1000000 --apply
```

工具会检查型号、当前 ID/波特率、目标 ID 冲突、扭矩和硬件错误；必要时关闭
扭矩，写后使用新配置读回验证，并在异常时恢复板内真实总线波特率。工具不会
自动重新使能扭矩或写入目标位置。

## 115200 文本 CLI

USART1 PA9/PA10 默认始终提供 115200 文本维护口；它与 USART2 软总线同时启用。
原来的 `-DebugCli` 参数仍可传入以兼容旧构建命令，但不再改变生成的固件：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 `
  -BuildDirectory build-ch32-cli -DebugCli
```

CMake 仍接受 `-DIMU_POWER_DEBUG_CLI=ON`，并提示该选项已弃用。

## 验证摘要

- Protocol 2.0 CRC、字节填充、坏帧恢复、DMA 环形边界和 softbus 队列：宿主机
  6/6 测试通过。
- 0x020A 的 PowerShell、CMake 和旧 `-DebugCli` 兼容入口生成相同 BIN：15,956
  字节，SHA-256 `3d132032302cfaab52e658bcc59df0d08435d2abacf9f8401c15d8cce023a51e`。
- 0x020A 已烧录且完整读回一致；PA2/PA3 软总线完成普通 READ 300/300 和
  SYNC_READ 100/100，PA9/PA10 CLI 的四项只读命令均通过。
- 外接 XL330-M288 后，0x020A 经 COM16 在 1 Mbps 找到 ID 3（型号 1200、固件
  53），并完整读取控制表状态，确认 USART2→USART3→外部设备回复链路可用。
- 综合 `check` 实测 IMU 20/20 样本及四元数有效、约 251.7 Hz，同时识别 ID 3
  的 `0x80` Alert、输入电压错误和保护/扭矩状态，扫描不会再被 Alert 中止。
- 已发布 0x0208 镜像烧录后完整读回一致，内部 IMU ready，样本约 247.7 Hz。
- 两台 XL330-M288 已验证 ID 1/2、1 Mbps；安全控制表访问 142/142 通过。
- `SYNC_READ(124,12,[200,1,2])` 50 Hz、10 分钟、30,000/30,000 成功；平均
  延迟 2.89 ms，最大 6.70 ms，`last_error=0`。
- 第三台 XL330-M288 已配置为 ID 3、1 Mbps；
  `SYNC_READ(124,12,[200,1,2,3])` 50 Hz 冒烟测试 100/100 成功。

尚未覆盖：0x020A 多外设长稳、真实断电持久化、IMU 物理断连恢复、看门狗
强制复位和硬件异常帧注入。

## 仓库结构

```text
include/imu_power/   可移植公共接口
src/                 协议、设备、配置、IMU 和 softbus 核心
platform/            CH32V203 外设适配
tests/               宿主机 C 测试
tools/               USART2 软总线 Python 工具
vendor/              固定版本 WCH 官方 SDK
docs/                架构、验证和发布记录
artifacts/            本地实机证据与可恢复临时归档（Git 忽略）
build-ch32/           标准 MCU 构建输出（Git 忽略）
build-host/           标准宿主机测试输出（Git 忽略）
```

文档导航见 [docs/README.md](docs/README.md)。第三方来源与许可证见
[THIRD_PARTY.md](THIRD_PARTY.md)、[provenance.json](provenance.json) 和
`vendor/wch-ch32v20x/manifest.json`。
