# WCH CH32V20x SDK

本目录随固件仓库一起提交，提供编译所需全部 WCH SDK 源码。构建不访问外部 SDK 路径，也不执行下载。

## 来源与版本

- 官方发布页：[CH32V20xEVT](https://www.wch.cn/downloads/CH32V20xEVT_ZIP.html)。
- 来源为本地已有的官方 `CH32V20xEVT` 解压包；包内 `CH32V20x_List_EN.txt` 标注版本日期 **2026.07**。
- `ch32v20x.h` 中的标准外设库版本为 **2.4**。
- 纳入日期：2026-09-11。
- `manifest.json` 记录这次纳入的全部 54 个 SDK 文件的原始长度和 SHA-256。清单中的路径相对于本目录。

## 纳入范围

保持官方 `EVT/` 目录结构，原样复制以下内容：

- `EXAM/SRC/`：完整 Core、Debug、Ld、Peripheral、Startup 公共源码和头文件。
- `EXAM/USART/USART_DMA/User/system_ch32v20x.c`、`.h`：当前固件使用的官方系统初始化实现。
- `CH32V20x_List.txt`、`CH32V20x_List_EN.txt`：原始包目录与版本说明。

无关的示例工程、RTOS、评估板 PCB/PDF 和 IDE 二进制不属于这份源码集合。该集合已覆盖本固件的全部 SDK 引用。

## 原始文件与使用声明

WCH 文件保持原始字节和版权头；`.gitattributes` 禁止 Git 对 `EVT/` 文件转换换行符，`build.ps1` 每次构建会核对清单哈希。版权及使用声明以各文件中的官方原文为准，其中声明软件及二进制用于南京沁恒微电子制造的微控制器。

固件保留最后 8 KiB Flash 用于配置，因此构建时将官方链接脚本复制到输出目录，再把应用区从 64 KiB 缩至 56 KiB；仓库内官方脚本保持原样。升级 SDK 时应同步更新来源、清单，并重新执行固件构建和宿主机测试。
