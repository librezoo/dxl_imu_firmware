# MicroDuck `imu_to_dxl` v2 功能报告

## 证据范围

本报告基于已下载仓库：

- [`microduck`](https://github.com/pollen-robotics/microduck)：板端 Rust runtime、Dynamixel 总线、控制、安全、健康监控。
- [`microduck_rl`](https://github.com/pollen-robotics/microduck_rl)：MuJoCo/PPO 训练环境和 sim2real 观测约定。

仓库中没有发现 `imu_to_dxl` v2 的独立 PCB/KiCad 原理图或板端固件工程；因此“芯片型号、完整控制表、PCB 电气细节”只能对代码明确使用的接口做确定性结论，对未公开部分单独标为推断。

我还获取了 `microduck` 的完整 Git 历史并检索了所有包含 `imu`、`dxl`、`SFLP`、`LSM6DSV16X` 的对象。历史中没有额外的板卡工程或不同寄存器定义；`duck-control/src/imu.rs` 从 Slice 1 引入后一直是当前实现的唯一解码源。因此，下面的“v2 接口”是仓库当前唯一可执行的硬件契约，而不是从旧版本拼接出的猜测。

## 一句话定义

`imu_to_dxl` v2 是一个挂在 Dynamixel 舵机总线上的智能 IMU 从设备：设备 ID 为 200，使用 Dynamixel Protocol 2.0，向主控提供固定 12 字节的角速度+SFLP 姿态数据块；主控把它和 15 个舵机放进同一次同步读取。它不是独立 USB/I²C 传感器。

## 关于“MicroDuck 有两个 IMU”的澄清

官方产品规格明确写的是 **2 个实体 IMU：一个在身体、一个在头部**。citeturn15view0turn14view0 但开源代码目前只公开并接入了身体 IMU 的 `imu_to_dxl v2` 路径：

- `kinematics/assets/alpha/robot_walk.xml` 中的 `imu_bno` 和 `head_imu` 都只是 MJCF `<site>` 几何标记，没有 `<sensor>`、总线 ID 或驱动调用。
- `microduck_rl` 的 `sensors.xml` 只给 trunk 的 `imu` site 挂载 `framequat`、`gyro`、`velocimeter`、`accelerometer`；没有 `head_imu` 传感器。
- `robotd`/`duck-control` 只有一个 `IMU_DXL_ID=200`，且每个 tick 只解析一个 12 字节 IMU block。

因此应区分两件事：**硬件规格是身体+头部两个 IMU；当前公开 runtime 的可验证通信/控制路径只有身体 IMU（Dynamixel 总线 ID 200）**。头部 IMU 的芯片型号、主控接口（I²C/SPI/UART/其他）、地址/ID、采样和用途，在当前两个仓库中没有实现或文档说明，不能臆测为第二个 Dynamixel 设备。

## 已由 runtime 确认的硬件/协议接口

### 总线

- 设备 ID：`200`（`duck-control/src/model.rs`）。
- 波特率：`1,000,000 bit/s`。
- 协议：Dynamixel Protocol 2.0。
- 物理拓扑：Radxa `/dev/ttyS2` 单 UART；IMU 板与 15 个舵机共享总线。
- 同步读取 ID 顺序：`[200, 20..24, 30..34, 10..14]`；IMU 放在第一个响应槽。
- 每个控制周期（50 Hz）一次 `sync_read`，地址 `124`、长度 `12`。
- 读取超时：30 ms；任一设备缺席会使组合读取失败。

### IMU 数据块（地址 124，12 字节）

| 字节 | 编码 | 语义 |
|---|---|---|
| 0..2 | little-endian `i16` | gyro X，±500 dps，17.5 mdps/LSB |
| 2..4 | little-endian `i16` | gyro Y |
| 4..6 | little-endian `i16` | gyro Z |
| 6..8 | IEEE-754 binary16 | SFLP quaternion X |
| 8..10 | IEEE-754 binary16 | SFLP quaternion Y |
| 10..12 | IEEE-754 binary16 | SFLP quaternion Z |

`w` 不在线传输，由 `w = sqrt(max(0, 1-x²-y²-z²))` 计算。输出四元数为主控坐标系的 scalar-first `[w,x,y,z]`。

代码位置：`microduck/duck-control/src/imu.rs`。

### 传感器坐标转换和滤波

- 默认安装旋转四元数：绕 Y 轴 +90°，使 trunk 坐标约为 `[+raw_z,+raw_y,-raw_x]`。
- 四元数先做传感器坐标到 trunk 的旋转，再归一化。
- 重力向量由四元数反向旋转世界重力 `[0,0,-1]` 得到。
- gyro 和 gravity 都使用三点逐分量中值，抑制单点尖峰。
- 全零 quaternion 被视为 SFLP 尚未初始化，保持上一次有效四元数，不重置为 identity。
- 收到至少 25 个有效 quaternion 样本后才 `imu_ready=true`，约 0.25 s（50 Hz 控制下约半秒内完成，具体取决于实际采样率）。

### 数据冻结检测

`StaleImuTracker` 对 12 字节原始块做逐块比较：

- 与前一块完全相同：累计 `total` 和连续 `run`。
- 新块到达：连续计数清零，总计数保留。
- 连续 25 次（约 0.5 s@50 Hz）开始告警；之后每 500 次限频告警。
- 该机制区分“总线通信成功但 IMU 不再刷新”和真正的新样本。

### 未使用但已在代码中声明的扩展数据

`imu.rs` 的模块注释声明板端完整诊断块为 20 字节，除上述 12 字节外还携带：

- 原始三轴加速度
- sample counter
- status flags

但是当前 `bus.rs` 固定以地址 124、长度 12 读取，且没有解析这 8 个扩展字节。因此扩展字段的确存在于软件设计说明中，但其确切字节布局、读地址和位定义不能从公开仓库确定；复刻时不能把它们当作已验证的兼容要求。

## 舵机总线相关功能（IMU 板参与的系统路径）

虽然这些寄存器属于舵机，但决定 IMU+舵机组合事务能否稳定运行：

- 启动时检查/修正 `return_delay_time=0`、`baud_rate=3`（1 Mbps）、`pwm_slope=255`、`shutdown=52`。
- `shutdown=52` 对应过载、过热、输入电压故障锁存。
- 每 20 ms 同步读取舵机电流、速度、位置以及 IMU 12 字节块。
- 每 20 ms 同步写入 15 个舵机目标位置。
- 每秒额外同步读取地址 `144..146`：输入电压（0.1 V/计数）和每个舵机温度。
- 电压取有效舵机读数平均值；温度保留逐关节值。

## Runtime 中所有实际调用点

### 控制策略观测

`duck-control/src/obs.rs` 将 IMU 放入 61 维 actor observation：

```text
gyro(3) | projected_gravity(3) | joint_pos(14) |
joint_vel(14) | last_action(14) | command(13)
```

嘴部关节不进入 14 维策略动作，因此 IMU 间接影响所有步态/站立策略，但不直接控制单个执行器。

### 跌倒检测与自恢复

- 当前重力方向用于判断倾斜。
- gyro 用于计算重力变化率 `g_dot = -(ω × g)`，预测持续倾倒。
- `Safety` 的“已跌倒”判定默认 `gravity_z > -0.5` 持续 200 ms；它是状态报告/门控条件，不直接替 limp-fall 写电机。
- `FallPredictor` 的提前触发条件是：已经倾斜到 `gravity_z > -0.90`（约 26°）、`g_dot_z > 0` 且 300 ms 线性外推超过 `-0.5`，连续 60 ms（50 Hz 下 3 tick）才触发。
- 检测需通过 IMU ready 门控和多周期去抖。
- 触发后执行 limp gain、等待 gyro 安静、约 1 s 插值回站立姿态，再交还站立策略。
- IMU 未 ready 或读取失败时，不允许用默认姿态做跌倒判断。

### 里程计

`odometry::Odometry::alpha()` 使用舵机位置、足端接触和 IMU 四元数估计相对位置与 yaw。yaw 是开机时的相对航向，没有磁力计校正，会漂移。

### 健康监控/IPC

`robot.health` 暴露：

- IMU ready 状态
- stale 总次数
- 连续 stale 次数
- 总线/控制循环速率和丢 tick
- 电压、舵机温度、板温度描述

IMU 相关数据会被 `robotctl monitor` 显示；状态帧主要输出 projected gravity，完整诊断通过 health 获取。

## RL 仓库如何依赖这块板的行为

`microduck_rl` 没有模拟 Dynamixel 数据包，而是模拟其最终语义：

- actor 使用 body-frame angular velocity（对应 v2 的 gyro）。
- actor 使用 projected gravity（对应 v2 四元数推导的 gravity）。
- 所有策略保持统一 61D 观测，便于 runtime 替换 walk、standup、ground-pick、kick、roulade、roller 等模型。
- 训练时对 IMU 安装误差做 actor-only 的 ±6° 随机旋转；critic 使用真值。
- IMU 观测延迟通常限制为 0–1 控制步（约 ±20 ms），说明真实 DXL IMU 路径被假定为低延迟。
- 仿真还加入 gyro/gravity 噪声和延迟，用于 sim2real 鲁棒性。

训练配置还保留了一个 `USE_PROJECTED_GRAVITY=False` 的可选路径，用原始加速度替代 projected gravity；这只是仿真/策略实验开关，当前 v2 runtime 的 `SflpDecoder` 不读取原始加速度，实际 actor 默认使用 projected gravity。

训练注释还记录真实板存在约 5° 的系统性 pitch 偏差，并称由 runtime 的 `imu-pitch-offset` 在源头修正；当前公开 `microduck` 主分支中没有这个参数或实现，只有固定的 `DEFAULT_MOUNT`（+90° Y 旋转）。因此“5° pitch offset 的具体校准值/寄存器”属于未公开或已移除功能，不能据此设计硬件寄存器。

相关文件：`microduck_rl/src/mjlab_microduck/tasks/microduck_velocity_env_cfg.py` 及各任务配置、`tasks/mdp.py` 的 `projected_gravity_imu_misaligned` 和 `base_ang_vel_imu_misaligned`。

## 从代码可反推的 v2 板内部结构

确定部分：板端存在能输出 SFLP quaternion 的 ST LSM6DSV16X（代码模块注释明确写出），以及一个将传感器数据映射为 Dynamixel 控制表的 MCU/协议处理器。

合理的工程框图是：

```text
Dynamixel 电源输入
   ├─ 降压到 MCU/IMU 电压
   ├─ 3.3 V TTL 半双工单线接口（非 RS-485）
   └─ MCU ── SPI/I²C ── LSM6DSV16X + SFLP
```

尚未被公开仓库证明的内容：MCU 精确型号、收发器型号、供电范围、PCB 层叠、连接器针脚、完整 20 字节诊断块定义、板端固件源码、EEPROM 控制表完整地址。

历史审计也未找到板端升级、参数写入、SFLP reset 或 IMU 校准命令的 Dynamixel 调用。换言之，MicroDuck 当前软件把 v2 当作“只读传感器从设备”：所有校准/融合主要在板端完成，主控只读数据并做坐标变换、去尖峰和健康判定。

## 复刻的最小兼容规格

要替代 MicroDuck 原板，第一版必须满足：

1. Dynamixel Protocol 2.0，1 Mbps，默认 ID 200。
2. 对地址 124 返回 12 字节：gyro XYZ（有符号 little-endian）+ quaternion XYZ（half float）。
3. gyro 比例 `0.0175 × π/180 rad/s/LSB`，量程 ±500 dps。
4. quaternion 的坐标系、符号、XYZ 顺序与 v2 一致。
5. 上电后 SFLP 未 ready 时返回全零 quaternion或等价未就绪状态；不要伪造 identity 姿态。
6. IMU 更新频率至少覆盖 50 Hz 控制循环，最好使用 FIFO/中断与总线响应解耦。
7. 支持连续样本刷新；必要时增加 sample counter/status 便于诊断。
8. 总线物理层支持与 15 个舵机并联/串联，半双工方向切换时序可靠。

## 硬件选型建议（复刻版）

### 推荐的身体 `imu_to_dxl` 节点

| 子系统 | 首选器件/方案 | 选型理由 |
|---|---|---|
| MCU | STM32G431CBT6 | 3.3 V、硬件 UART/SPI、DMA/FPU，足够处理 1 Mbps DXL、SFLP 数据和诊断；比带 Wi‑Fi 的 MCU 更容易做确定性时序 |
| IMU | **ST LSM6DSV16X** | 与公开代码完全一致；1.71–3.6 V，支持 SPI/I²C，板端使用其 SFLP 四元数 |
| DXL 物理层 | **3.3 V TTL 半双工单线**，MCU TX 经 74LVC1G125（三态）接到 DATA，MCU RX 监听同一 DATA | XL330 系列使用 TTL 多点总线，不应默认选 RS‑485 收发器；TX 使能脚负责收发方向切换，DATA 端放 10–22 Ω 串联电阻并做好 ESD |
| 电源 | 舵机母线输入后用同步降压得到 5 V，再用低噪声 3.3 V LDO 给 MCU/IMU | XL330 工作电压约 3.7–6 V；IMU 不应直接接舵机电源 |
| 连接器 | JST EHR-03/B3B-EH-A 同类 3 针（VDD/GND/DATA） | 与 XL330 线束生态兼容；电源线按舵机峰值电流留裕量 |
| 调试 | SWD、独立 USB-UART/测试焊盘、状态 LED | 不能占用 DXL UART 的启动/调试功能 |

MCU 内部建议用 SPI 连接 LSM6DSV16X（约 8–10 MHz），用 IMU 中断/FIFO 驱动采样；对外严格实现 `ID=200、1 Mbps、Protocol 2.0、地址 124 返回 12 字节`。第一版不要把未公开的 20 字节诊断扩展当作必选项。

### 头部第二个 IMU 的选型

MicroDuck 的物理规格是“身体 1 个 + 头部 1 个 IMU”，但公开运行时只发现身体 `imu_to_dxl`（ID 200）的读取；头部 IMU 的主机接口和寄存器协议没有公开。因此建议：

1. 头部仍选同一颗 LSM6DSV16X，保证轴向、噪声和标定流程一致。
2. 头部优先做成“IMU + 小 MCU（STM32C031K6/G031）”本地模块，通过短线 SPI/I²C 连接；不要在没有协议证据时直接假定它是第二个 DXL 节点。
3. 若需要让现有主控读取头部，后续可将头部模块映射成第二个 DXL 设备（例如 ID 201），但必须同步修改 `duck-control` 的设备列表、sync-read 和上层观测；仅增加硬件而不改软件，头部数据不会被使用。

### 分阶段落地

- **A：可替换身体板**：G431 + LSM6DSV16X + TTL 半双工 DXL，先通过 MicroDuck 现有驱动的 12 字节读验证。
- **B：双 IMU 硬件**：增加头部 LSM6DSV16X 和本地 MCU，定义头部帧、时间戳和健康状态。
- **C：软件融合**：扩展总线读取 ID、头部/身体坐标外参和故障降级；在此之前只把身体 IMU用于闭环。

可选兼容项：20 字节扩展诊断块、板温/输入电压、校准命令、SFLP reset、LED/按键。

## 与公开 Bit-Bots IMU-DXL 模块的关系

Bit-Bots 的公开模块提供 ESP32+MPU6500、Dynamixel 2.0、互补滤波、量程配置、四元数、校准、LED 和按键，是非常好的电气/协议参考；但它的寄存器地址和 MPU6500 不能直接当作 MicroDuck v2 的真实定义。MicroDuck v2 的确定实现应以本报告中的 `address 124 / 12-byte / LSM6DSV16X / SFLP / ID 200` 为准。

## 结论

在 MicroDuck 中，`imu_to_dxl v2` 不是“显示姿态的附件”，而是控制闭环的实时总线设备。其完整职责包括：采样、SFLP 姿态融合、坐标转换、角速度输出、样本有效性/冻结诊断、跌倒预测、自恢复门控、策略观测、里程计和健康上报。复刻时最关键的是协议数据语义和时序，而不是外观或某个通用 IMU 模块的寄存器表。

## 仓库能否证明板上还有其他电路？

不能从仓库得到完整原理图或 BOM：仓库只包含主机端 Rust 软件、仿真模型和部署脚本，没有 `kicad`、原理图、PCB 或元件清单。`imu.rs` 中的“one IMU, one code path”是软件抽象，不等于板上只有一颗 IMU。

代码可以确定或强烈暗示板上至少还必须有：

- MCU：执行 SFLP 配置/数据搬运、四元数表维护和 Dynamixel Protocol 2.0 从设备响应；精确型号未公开。
- TTL 半双工 Dynamixel 接口电路：板子以 ID 200 与 15 个舵机共用一条 UART 总线；精确收发器/三态缓冲器型号未公开。
- 电源管理与去耦：外部来自舵机总线，IMU/MCU 需要独立低压供电；具体稳压器、保护器件和额定范围未公开。
- IMU 辅助电路：去耦、SPI/I²C 走线、可能的中断/FIFO 连接；代码明确使用 LSM6DSV16X 的 SFLP 输出。

代码还提到一个 20 字节诊断块，包含原始加速度、sample counter 和 status flags，但没有公开其完整寄存器定义；这不能据此推断板上还有独立加速度计。LED、按键、EEPROM、温度/电压监测等在仓库中没有被证明属于 `imu_to_dxl v2` 板。

官方资料把相机、LiDAR、麦克风、扬声器和 NFC 列为整机部件，并明确说机械/电子设计文件不开放；这些不能归到 `imu_to_dxl` 板上。官方资料同时确认整机有身体和头部两个 IMU，但开源运行时目前只访问身体的 ID 200 节点。

## 将电源监测加入 `imu_to_dxl v2`（重新选型）

如果目标是让 IMU 板同时输出电源信息，建议把它定义成“IMU + DXL 从设备 + 总线电源监测”板，而不是只在 3.3 V 侧测量。电流测量必须让舵机供电路径经过板上的分流器，否则只能得到板自身电流，不能代表整机负载。

### 推荐器件

| 功能 | 推荐器件/电路 | 设计要点 |
|---|---|---|
| 主控 | STM32G431CBT6 | 负责 DXL 1 Mbps、IMU SPI、功率监测 I²C、滤波和寄存器响应 |
| 身体 IMU | LSM6DSV16X | 保持原有 SFLP/12 字节兼容 |
| 总线电压/电流监测 | INA238（16-bit，I²C）+ 5 mΩ、≥3 W 四端分流器 | 高侧测量；建议按至少 20 A 瞬态设计，实际分流阻值应按实测峰值和发热校核 |
| 输入电压保护 | 反接 MOSFET、保险丝/可恢复保险丝、低电容 TVS、浪涌/欠压设计 | NP-F550 2S 输入可能高于舵机允许电压，不能直接把电池接到 XL330 总线 |
| 舵机电源 | 同步 Buck 输出约 5.0 V，额定电流按舵机峰值留裕量 | 具体电流等级必须通过 15 舵机堵转/启动实测确定；不要仅按平均功耗选型 |
| MCU/IMU 电源 | 5 V → 3.3 V 低噪声 LDO，独立去耦 | INA238 可接受总线侧电压，数字 I²C 仍使用 3.3 V 电平 |
| 温度 | INA238 板载/外接 NTC（二选一） | 至少监测分流器和 Buck 热点；这不是原舵机温度数据的替代品 |

### 建议的数据表扩展

保留原有 `ID=200, address=124, length=12`，确保旧版主机仍能工作。由于代码提到原板可能有 `124–143` 的 20 字节诊断区，电源寄存器应从 144 开始，避免覆盖未知扩展，例如：

| 地址 | 长度 | 数据 | 单位 |
|---:|---:|---|---|
| 144 | 2 | VBUS 输入电压 | 10 mV/LSB |
| 146 | 2 | 舵机侧 5 V 电压 | 10 mV/LSB |
| 148 | 4 | 舵机侧电流 | 1 mA/LSB，有符号/无符号需固定 |
| 152 | 2 | 板温或分流器温度 | 0.01 °C/LSB |
| 154 | 2 | 电源状态位、采样计数器 | bit/status |

这样可以新增一个 `address=144, length=12` 的 sync-read，而不改变旧主机读取的 IMU 数据。若希望一次事务读完，可把 IMU 块扩展为 32 字节，但必须同步修改主机 `READ_LEN`、带宽预算和所有解析代码，不建议作为第一版兼容方案。

### 关键硬件约束

1. 电压采样点应同时放在“电池输入”和“舵机 Buck 输出”两侧，才能区分电池掉压、Buck 限流和舵机端压降。
2. INA238 的分流器要放在所有舵机电流必经的高侧路径；若舵机从另一条线取电，读数将失真。
3. DXL DATA 仍是 3.3 V TTL 半双工单线，不因加入 INA238 就改成 RS-485。
4. 电源采样使用独立 I²C 地址和 CRC/状态位；发生 ADC 饱和、分流器过温或 Buck 欠压时，寄存器必须置 fault，而不是输出 0 V。
5. 主机端应以电压作为电池欠压依据，以电流作为负载/堵转诊断；不能仅凭电压计算真实剩余容量。

### 重新选型后的最小 BOM

`STM32G431CBT6 + LSM6DSV16X + INA238 + 5 mΩ 四端分流器 + 5 V 大电流同步 Buck + 3.3 V LDO + TTL 半双工 DXL 接口 + 输入保护/TVS + SWD`。

头部第二个 IMU 仍建议采用 `LSM6DSV16X + STM32C031K6` 本地模块；它不应直接复用身体板的电源寄存器，除非后续把头部节点也纳入 DXL 地址和同步读取协议。

> **需求更正：仅加入 DC-DC 与供电，不加入电源监测。** 上述 INA238、分流器、NTC 和电源寄存器方案属于“电源监测版”，不适用于当前需求；复刻板应删除这些器件和协议扩展。

### 仅供电版的推荐电源架构

```text
NP-F550 电池输入（约 6.0–8.4 V）
        │
  保险丝/反接保护/TVS
        │
  大电流同步 Buck（输出 5.0 V 舵机母线）
        ├── Dynamixel TTL DATA + 5 V + GND
        └── 3.3 V 低噪声 LDO/Buck → STM32G431 + LSM6DSV16X
```

推荐使用 5 V 舵机母线，因为 XL330 类舵机工作电压约为 3.7–6 V；电源转换器应按 15 个舵机的启动/堵转峰值选额定电流，不能按平均功耗估算。可选电源芯片为 TPS62160/TPS62162（中等电流）或更高电流的 MPQ/JW 系列同步 Buck；最终型号必须以实测峰值、电感温升和 PCB 散热验证为准。板上不放 INA238、分流电阻、电流 ADC 或电源监测寄存器。

### 可直接画原理图的核心 BOM（外围无源件暂不列）

| 位号/功能 | 推荐 MPN | 封装/接口 | 备注 |
|---|---|---|---|
| U1 MCU | STM32G431CBT6 | LQFP-48 | 3.3 V，UART/SPI/I²C/DMA/FPU |
| U2 身体 IMU | LSM6DSV16XTR | LGA-14 | 与 v2 软件数据语义一致 |
| U3 舵机 Buck | MPQ8633AGLE-Z | QFN | 12 A 级同步 Buck，输入覆盖 2S 电池，输出设为 5.0 V |
| U4 逻辑 LDO | TLV75533PDBVR | SOT-23-5 | 5 V 转 3.3 V，MCU/IMU 逻辑电源 |
| U5 DXL TX 三态缓冲 | SN74LVC1G125DBVR | SOT-23-5 | 3.3 V 供电，OE 由 MCU 控制半双工方向 |
| J1 电池输入 | XT30U-M（板端）或 JST VH B2P-VH | 2 针 | 选型取决于 NP-F550 电池线束；原型建议 XT30 带线版本 |
| J2/J3 DXL 总线 | B3B-EH-A（板端） | JST EH，3 针 | 2 个并联端口，分别标 DXL_IN/OUT；匹配 EHR-03 外壳和 SEH-001T-P0.6 端子 |
| J4 调试口 | Samtec FTSH-105-01-L-DV-K | 2×5，1.27 mm | ARM Cortex 10-pin SWD，保留 NRST/SWV |
| J5 3.3 V/串口测试口 | 1×4，2.54 mm 排针 | GND/3V3/SWD-UART_TX/SWD-UART_RX | 仅调试，不接 DXL 总线 |

XL330 官方连接器就是 JST EHR-03 与板端 B3B-EH-A，通信为 3.3 V TTL；因此 J2/J3 不要换成 RJ45、PH2.0 或 RS-485 接口。 [XL330 connector information](https://emanual.robotis.com/docs/en/dxl/x/xl330-m077/)

### MCU 算法负担与 RISC-V 替代

板上 MCU 不需要运行强化学习、EKF 或复杂 PID。LSM6DSV16X 内部的 SFLP 负责主要姿态融合；MCU 负责配置传感器、读取 gyro/quaternion、维护 50 Hz 数据表、实现 1 Mbps Dynamixel 半双工协议、看门狗/超时和电源上电时序。STM32G431 的 FPU/DSP 是裕量而非必需。

因此可用 RISC-V 替代：首选 CH32V203C8T6（144 MHz、64 KB Flash、20 KB SRAM、UART/SPI/I²C/DMA），但需要重写 STM32 HAL、DXL 协议栈和 LSM6DSV16X 驱动，并验证 1 Mbps 方向切换时序。若以“最快做出兼容板”为目标，仍选 STM32G431CBT6；若以成本和国产供应链为目标，选 CH32V203C8T6。
