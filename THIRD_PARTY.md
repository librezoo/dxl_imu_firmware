# 来源与依赖

`provenance.json` 保存本次拆分所用文件的原始路径和整理前哈希。该清单表达本地提取来源，不将第三方实现认定为原创。

来自 Pollen Robotics Microduck 的改动基于提交 `590b986bd8c0d50ae02cb3ea2f59c463b6828168`；对应上游许可在 `LICENSES/` 中保留。

固件编译所需 WCH CH32V20x EVT SDK 已随仓库提交到 [vendor/wch-ch32v20x](vendor/wch-ch32v20x/README.md)，来自官方 EVT 包 2026.07，标准外设库版本为 2.4。逐文件原始 SHA-256 见该目录的 `manifest.json`。SDK 文件保留 WCH 原始版权及使用声明，包括仅用于 WCH 微控制器的声明；上述 Microduck 的 Apache-2.0 许可不替代 WCH 文件中的声明。

编译工具由官方 MounRiver Studio 安装提供，路径和命令见根目录 README。固件构建无需下载外部 SDK、依赖源码或子模块。
