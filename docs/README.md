# 文档索引

## 当前实现与放行依据

- [固件架构与构建](firmware-details.md)：硬件平台、Flash/RAM 布局、构建入口、
  IMU 数据语义和 DEBUG 软总线实现。
- [累计验证记录](validation.md)：各版本修复、构建和实机验证的时间线。
- [0x0208 DEBUG 软总线实机验证](hardware-softbus-2026-09-17.md)：烧录、回显
  修复、1 Mbps DEBUG、舵机扫描/配置、控制表访问和 30,000 周期长稳测试。

## 专项调查

- [UART 与合并读取恢复](uart-recovery-2026-09-16.md)：DMA 环形索引、坏帧恢复、
  CRC/字节填充和首包截断问题。
- [0x0202 烧录与只读总线验证](hardware-validation-2026-09-16.md)：WCH-Link
  烧录、Flash 读回和 5,000 次合并读取。
- [IMU 量程与静态验证](sensor-validation-2026-09-16.md)：±500 dps、SFLP
  保留位、采样年龄、偏置和静态数据。
- [240 Hz 与总线负载](rate-and-bus-load-2026-09-17.md)：200 Hz IMU、50 Hz
  舵机混合读取和线速占用。

## 发布记录

- [firmware-20260918-0x0209 / DEBUG RX 空闲上拉修复](releases/firmware-20260918-0x0209.md)
- [0x0209 请求回流修复验证](debug-rx-idle-fix-2026-09-18.md)
- [firmware-20260917-0x0208 / DEBUG 软总线](releases/firmware-20260917-0x0208.md)
- [firmware-20260917 / 0x0207](releases/firmware-20260917.md)
- [firmware-20260916 / 0x0206](releases/firmware-20260916.md)

当前版本为 `firmware-20260918-0x0209`；旧版本说明保留各自的历史验证范围。

## 历史背景

- [旧接口与硬件分析](legacy-interface-report.md)：早期 MicroDuck 协议调查与
  方案比较，仅作历史参考，不替代当前 EasyEDA 网表和实机证据。

本地原始日志、Flash 读回、测试 JSON、临时工具和旧构建目录保存在 Git 忽略的
`artifacts/` 下；标准可重建输出仅使用根目录 `build-ch32/` 和 `build-host/`。
