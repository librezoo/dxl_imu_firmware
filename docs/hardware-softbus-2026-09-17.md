# 0x0208 DEBUG 软总线烧录与实机验证

日期：2026-09-17。目标为当前复刻 `CH32V203C8U6 + LSM6DSV16X` 板卡；本页
记录本机烧录与只读验证，不代表机器人运动验收。

## 烧录与保护状态

- 下载器：WCH-LinkE v2.18；目标 ChipID `0x20300500`，Flash 64 KiB。
- 芯片开始时启用了读保护，OpenOCD 读回全零，不能作为备份。经用户明确许可
  解除保护；该操作整片擦除了旧应用和末尾 8 KiB 配置。
- 解除保护后配置页为擦除/未初始化模式，固件回退默认 ID 200、真实总线
  1 Mbps。Flash 读保护目前保持关闭。
- 最终默认桥接镜像为 14,324 字节，SHA-256：
  `b4f4f92a937f042925bfd321f7eb1e1b2ff490c752aeab0372211442f586b0aa`。
- 最终程序区读回与 BIN 逐字节一致；随后执行 PFIC 系统复位并恢复运行。
- 调试器只读 RAM 确认最终 1 Mbps 镜像的 `imu_ready=1`，采样时钟持续更新。

备份、读回和测试日志保存在忽略目录 `artifacts/softbus-hw-20260917/`。

## 实机发现与修复

WCH-LinkE 的 UART 官方最高波特率是 921600。以 1 Mbps 打开 COM9 时，PA10 DMA
收到系统性失真字节；改用 921600 测试镜像后，14 字节请求逐字节正确。

测试还确认本板 USART3 RX 不监听 MCU 自己的 TX。原实现依赖请求物理回显来驱动
内部 IMU，因此没有回复且回显匹配器不会结束。最终实现改为：请求仍原样发到
真实总线，同时直接送入同一本机 DXL 解析器；内部 IMU 实际发送的状态包原样加入
DEBUG TX 队列。物理请求回显若存在仍会被抑制，且不重复解析。

## 内部 IMU 软总线结果

为适配 WCH-LinkE 串口，使用仅将 DEBUG UART 改为 921600 的临时测试镜像；真实
Dynamixel 总线仍为 1 Mbps。验证完成后已重新烧录正式 1 Mbps 镜像。

- 地址 160、长度 24 的诊断读取返回完整 35 字节状态包，CRC 正确。
- 诊断版本 `0x0208`、协议 2、ID 200、总线波特率枚举 2、状态 `0x0003`。
- `0x0003` 表示 IMU 样本有效且初始化 ready；`last_error=0`，样本年龄 3 ms。
- 连续读取地址 124、长度 12 共 500 次：500 成功、0 超时、0 CRC/设备错误。
- PowerShell 测试器实测速率约 63.24 Hz；500 次中得到 492 个不同线回复，确认
  IMU 数据持续更新。该速率受测试器轮询开销限制，不是总线能力上限。

## 外接舵机初始 1 Mbps 扫描

- 对 MicroDuck 预期 ID `10–14、20–24、30–34` 执行只读 PING，均无回复。
- 随后在 1 Mbps 扫描 ID `0–253`，唯一响应者仍为内部 IMU ID 200。
- 没有写入舵机 ID、波特率、扭矩、位置或其它寄存器，也没有执行运动测试。

后续接入 FTDI COM3，正式 1 Mbps DEBUG 接口返回版本 `0x0208`、状态 `0x0003`
的正确诊断包。FTDI 默认 USB 延迟使 15–20 ms 的 PowerShell 接收窗口过紧；使用
50 ms 窗口并丢弃首次预热事务后，连续读取 200/200 成功，零 CRC/设备错误，
测试器实测速率约 53.91 Hz。1 Mbps 扫描无舵机回复随后被确认是舵机波特率不同，
而非桥接链路失效。

## 外接舵机波特率扫描与联合读取

保持 DEBUG UART 为 1 Mbps，通过内部维护接口依次切换真实总线允许波特率，并在
每个波特率扫描 ID `0–253`；每轮完成后均由内部诊断读回确认配置，最终恢复
真实总线 1 Mbps。

| 真实总线波特率 | 外部响应 |
|---:|---|
| 57,600 | ID 1，模型号 1200 |
| 115,200 | 无 |
| 1,000,000 | 无 |
| 2,000,000 | 无 |
| 3,000,000 | 无 |

对 ID 1 的只读寄存器结果：固件版本 52、ID 1、波特率寄存器 1、硬件错误 0、
当前位置 439、输入电压 51（约 5.1 V）、温度 30°C。

真实总线临时设为 57,600 时，DEBUG 端以 1 Mbps 发送
`SYNC_READ(124,12,[200,1])`，收到两个 23 字节状态包：

- 内部 IMU ID 200：CRC 正确、error 0。
- 外接舵机 ID 1：CRC 正确、error 0。

这验证了 DEBUG 1 Mbps 到不同速率真实总线的转发、内部设备注入、外部设备回复
镜像和多设备同步读。测试未写舵机 ID、波特率、扭矩或目标位置，也未产生运动。
此阶段结束时板内真实总线恢复 1 Mbps，舵机仍为 57,600；随后按下一节的用户
指令修改舵机配置。

## 舵机最终配置为 ID 2 / 1 Mbps

用户随后明确要求将该舵机改为 1 Mbps、ID 2。写入前再次执行保护性检查：

```text
model=1200 id=1 baud_register=1 torque=0 hardware_error=0
```

确认型号、旧 ID、旧波特率和硬件状态全部符合预期，且扭矩已经关闭后：

1. 在真实总线 57,600 下将地址 7 写为 ID 2，并以 ID 2 读回确认。
2. 向 ID 2 的地址 8 写入值 3，将波特率改为 1 Mbps。
3. 将板内真实总线恢复 1 Mbps，再读回型号、ID、波特率、扭矩和错误状态。

最终读回：

```text
model=1200 id=2 baud_register=3 torque=0 hardware_error=0
```

随后通过 COM3 1 Mbps 连续发送
`SYNC_READ(124,12,[200,2])` 100 次，100/100 次均同时收到内部 IMU ID 200 和
舵机 ID 2 的状态包，顺序正确、CRC 正确、error 0。测试没有使能扭矩，也没有
写入目标位置；最终板内真实总线和舵机均保持 1 Mbps。

## 第二个舵机保持 ID 1 并改为 1 Mbps

接入第二个舵机后重新扫描全部允许波特率，确认设备分布为：

- 新舵机：型号 1200、ID 1、57,600。
- 现有舵机：型号 1200、ID 2、1 Mbps。

按用户要求保持新舵机 ID 1 不变，只修改波特率。写入前读回：

```text
model=1200 id=1 baud_register=1 torque=0 hardware_error=0
```

确认扭矩已关闭、硬件错误为 0 后，只向地址 8 写入值 3；地址 7 未写入。切换
板内真实总线到 1 Mbps 后读回：

```text
model=1200 id=1 baud_register=3 torque=0 hardware_error=0
```

最终 ID 1 和 ID 2 均能在 1 Mbps PING。通过 COM3 连续发送
`SYNC_READ(124,12,[200,1,2])` 100 次，100/100 次均按顺序收到 IMU ID 200、
舵机 ID 1、舵机 ID 2 的三个状态包，CRC 正确、error 0。没有使能扭矩或写入
目标位置。

## 两台舵机完整控制表访问测试

依据 XL330-M288 官方控制表，对 ID 1 和 ID 2 执行逐字段访问测试。型号、固件、
ID、波特率、Return Delay、Operating Mode、PWM Slope、Shutdown、Torque、
Profile/Goal Position 和 124–146 状态字段已有前序读写证据，本轮按用户要求跳过。

最终安全测试策略：

- 7 个新增只读字段逐项读取。
- 24 个普通 RW 字段保存原值，写入安全临时值或同值，读回确认，再恢复原值并
  二次读回；每个字段后检查 Torque Enable=0、Hardware Error=0。
- 固件 V52 的 20 个 Indirect Address 和 20 个 Indirect Data 全部逐项读取。
  间接区写入存在非直观映射副作用，因此最终安全版本不写间接区。
- Goal PWM、Goal Current、Goal Velocity 与此前的 Goal Position 一并按控制目标
  寄存器跳过写测试。

测试中发现两项需保留的安全证据：

1. Goal PWM 写入非零值或重新写入当前非零目标会自动将 Torque Enable 置 1，
   并曾锁存 `0x20` 过载错误；测试立即停止、恢复目标值、关闭扭矩并重启清错。
2. 固件 V52 的 Indirect Address 读回与 Indirect Data 实际映射行为不一致，聚焦
   写探针曾意外置位 Torque Enable。映射、LED 和扭矩均已恢复，最终版本只读。

最终结果：

```text
ID 1: 71/71 PASS, torque=0, hardware_error=0
ID 2: 71/71 PASS, torque=0, hardware_error=0
合计: 142/142 PASS
测试后 SYNC_READ(124,12,[200,1,2]): 100/100 PASS
```

结构化结果保存在
`artifacts/softbus-hw-20260917/full-servo-register-test-result.json`，SHA-256 为
`ea3c82d13d7080b192e0eb4e445e321c973551b0e2797e2640834d07043cb85b`。

## 0x0208 三设备 50 Hz 长稳测试

通过 COM3 对 `SYNC_READ(124,12,[200,1,2])` 执行 10 分钟固定 50 Hz 测试。
最初使用 FTDI VCP 默认 `LatencyTimer=16 ms`，30,000 周期中有 193 次未能在
19 ms 周期窗口内收齐三包；固件诊断始终正常，平均主机观测延迟约 18 ms，表明
失败来自 USB 缓冲贴近周期边界。

系统拒绝以非管理员权限修改 FTDI 注册表，因此改用已安装的 D2XX DLL，在单次
会话内设置 1 ms latency，不改变系统设置。60 秒冒烟测试 3,000/3,000 成功后，
正式 10 分钟结果为：

```text
cycles=30000
ok=30000
failed=0
actual_rate=50.00085 Hz
average_latency=2.8878 ms
max_latency=6.6958 ms
unique_imu_payloads=29997
max_consecutive_identical_imu_payloads=0
```

测试前后固件版本均为 `0x0208`，状态 `0x0003`、`last_error=0`；IMU 样本计数
从 279,382 增至 427,976，约 247.7 Hz，样本年龄结束时为 2 ms。结果文件：
`artifacts/softbus-hw-20260917/soak-three-devices-result.json`，SHA-256：
`5661e4ed53149ed54f6d2b8f2972bc6d187a755ad15f84fb9fc3ee3945de3935`。
