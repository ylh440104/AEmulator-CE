# AEmulator CE

在 Android 手机上运行其他 Android 固件。

本项目基于 [uxazu/AEmulator](https://github.com/uxazu/AEmulator)（GPL-3.0）修改，增加了对新固件格式的支持和若干缺陷修复。

## 支持的固件

| 容器 | 说明 |
|---|---|
| `payload.bin` | A/B OTA，含 `system`、`vendor`、`product`、`system_ext`、`odm` 及厂商扩展分区 |
| `system.new.dat` + `system.transfer.list` | 块 OTA，支持被切分成多片的 `.dat` |
| `super.img` | 动态分区（Android 10+） |
| `UPDATE.APP` | 华为 / 荣耀 SD 卡整包 |
| `KDZ` / `DZ` | LG |
| `PAC` | 展讯 |
| `SIN` | 索尼 `.ftf` |
| ZIP / 7z / RAR / TAR | 含嵌套归档 |

文件系统：ext4、EROFS（含多设备）、YAFFS2、FAT/RFS、sparse。

系统版本覆盖 Android 2.x 到 17。

## 安装

从 [Releases](https://github.com/ylh440104/AEmulator-CE/releases) 下载 APK 安装，或使用 GitHub Actions 自动构建的产物。

## 构建

```bash
git clone https://github.com/ylh440104/AEmulator-CE.git
cd AEmulator-CE
./gradlew assembleStandardDebug
```

产物在 `app/build/outputs/apk/`。

需要 JDK 21 和 Android SDK 36。

## 导入固件

1. 准备好固件压缩包或 `payload.bin`
2. 在应用里选择"导入"，选中文件
3. 等待解包和校验完成
4. 在列表里点启动

导入过程需要约为固件解压后 1.5 倍的临时空间。

## 自动构建

仓库配置了 GitHub Actions：

- `build-apk.yml`：推送代码后自动编译 APK
- `probe-firmware.yml`：手动触发，探测任意固件结构，不需要下载到本地
- `honor-firmware.yml`：荣耀 SD 卡包专用探测

## 许可

GPL-3.0，与原项目一致。见 [LICENSE](LICENSE)。

## 致谢

原始项目：[uxazu/AEmulator](https://github.com/uxazu/AEmulator)
更早的源头：[SENSES](https://github.com/Istratii-tech/SENSES)
