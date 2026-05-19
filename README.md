# DEV-TOOLS-SUITE

SylixOS 开发辅助工具集，包含板卡调试、文件上传、寄存器读取等常用小工具。

## 目录概览

| 路径 | 说明 |
|------|------|
| `telnet_interrupt_monitor/` | 通过 Telnet 周期性监测目标 IRQ 中断计数 |
| `ftp_sylixos_upload/` | 将文件上传到 SylixOS 板卡 |
| `readreg/` | 在 Linux 或 SylixOS 下读取寄存器值，用于调试和对比 |

---

## telnet_interrupt_monitor

通过 Telnet 循环连接指定设备，定期执行 `ints` 命令监测目标 IRQ 的中断次数。如果两次计数的值相同，则弹出系统警告（表示该 IRQ 可能停止了）。

> ⚠️ **仅支持 Windows 环境**（使用 Windows API 弹窗）

### 使用方法

修改脚本末尾的默认配置：

```python
TARGET_HOST = "10.13.21.42"      # 目标设备 IP
LOGIN_USERNAME = "root"         # 用户名
LOGIN_PASSWORD = "root"         # 密码
EXECUTE_COMMAND = "ints"        # 查询命令
TARGET_IRQ_NAME = "uart2_isr"   # 要监测的 IRQ 名称
CYCLE_INTERVAL_MINUTES = 1     # 轮询间隔（分钟）
```

直接运行脚本：

```bash
./telnet_interrupt_monitor/telnet_interrupt_monitor.py
```

按 `Ctrl+C` 可手动终止。

---

## ftp_sylixos_upload

FTP 上传工具，用于手动上传文件到 SylixOS 板卡。

> ⚠️ **主要在 Linux 环境下使用**

### 使用方法

```bash
# 自动解析项目 .reproject 并上传（推荐）
./ftp_sylixos_upload/ftp_sylixos_upload.py -P /path/to/project

# 使用当前目录的 .reproject
./ftp_sylixos_upload/ftp_sylixos_upload.py -P .

# 指定板卡 IP（覆盖 .reproject 中的配置）
./ftp_sylixos_upload/ftp_sylixos_upload.py -P . -i 10.13.21.100

# 上传单个文件
./ftp_sylixos_upload/ftp_sylixos_upload.py -i 10.13.21.42 -f lyn_drv.ko -t /lib/modules/drivers/lyn_drv.ko

# 上传到指定目录（保持文件名）
./ftp_sylixos_upload/ftp_sylixos_upload.py -i 10.13.21.42 -f liblyn_drv.so -d /lib/

# 使用自定义凭证
./ftp_sylixos_upload/ftp_sylixos_upload.py -i 10.13.21.42 -u admin -p admin123 -f test.ko -t /lib/modules/test.ko

# 批量上传（使用配置文件）
./ftp_sylixos_upload/ftp_sylixos_upload.py -i 10.13.21.42 -c upload_list.txt
```

### 参数说明

| 参数 | 说明 |
|------|------|
| `-P, --project` | 项目目录路径（自动解析 .reproject） |
| `-i, --ip` | 板卡 IP 地址 |
| `-u, --user` | FTP 用户名（默认: root） |
| `-p, --password` | FTP 密码（默认: root） |
| `-f, --file` | 本地文件路径 |
| `-t, --target` | 目标文件路径（完整路径） |
| `-d, --dir` | 目标目录（保持原文件名） |
| `-c, --config` | 配置文件（每行格式: 本地路径\|目标路径） |

---

## readreg

用于在 Linux 或 SylixOS 命令行下直接读取寄存器值，便于对比两边同一寄存器地址的实际内容。

### 文件说明

| 文件 | 说明 |
|------|------|
| `readreg/readreg_linux.c` | Linux 版本，通过 `/dev/mem + mmap` 读取物理地址 |
| `readreg/readreg_sylixos.c` | SylixOS 版本，通过直接解引用寄存器地址读取 |

### 编译方式

Linux：

```bash
gcc -O2 -Wall -Wextra -o readreg_linux readreg/readreg_linux.c
```

SylixOS：

```bash
${CROSS_COMPILE}gcc -O2 -Wall -Wextra -o readreg_sylixos readreg/readreg_sylixos.c
```

也可以将 `readreg/readreg_sylixos.c` 复制到 SylixOS IDE 工程中编译。

### Linux 使用方法

```bash
# 读取单个 32 位寄存器（默认 32 位）
./readreg_linux 0xfdc60068

# 按 16 位读取
./readreg_linux 0xfdc60068 16

# 连续读取 4 个 32 位寄存器
./readreg_linux 0xfdc60068 32 4
```

### SylixOS 使用方法

```bash
# 读取单个 32 位寄存器（默认 32 位）
./readreg_sylixos 0xfdc60068

# 按 16 位读取
./readreg_sylixos 0xfdc60068 16

# 连续读取 4 个 32 位寄存器
./readreg_sylixos 0xfdc60068 32 4
```

### 参数说明

`readreg_linux`：

| 参数 | 说明 |
|------|------|
| `phys_addr` | 要读取的物理寄存器地址，支持十进制或 `0x` 前缀十六进制 |
| `width` | 访问位宽，可选 `8`、`16`、`32`、`64`，默认 `32` |
| `count` | 连续读取的寄存器个数，默认 `1`。每次按 `width / 8` 递增地址 |

`readreg_sylixos`：

| 参数 | 说明 |
|------|------|
| `addr` | 寄存器地址，支持十进制或 `0x` 前缀十六进制 |
| `width` | 访问位宽，可选 `8`、`16`、`32`、`64`，默认 `32` |
| `count` | 连续读取的寄存器个数，默认 `1`。每次按 `width / 8` 递增地址 |


### 注意事项

- `readreg_linux` 依赖 `/dev/mem`，某些 Linux 内核启用了严格的 `/dev/mem` 访问限制，程序即使编译成功也可能无法读取目标地址。
- `readreg_linux` 适用于 Linux 下的 MMIO 物理地址读取，不适用于 I2C、SPI 等非直接物理映射寄存器。
- `readreg_sylixos` 的前提是当前 SylixOS 环境允许像 `*(volatile unsigned int *)0xfdc60068` 这样直接访问目标地址。
- 某些寄存器存在“读清零”或其他读副作用，读取前需要先确认芯片手册。
- 地址和访问位宽应与寄存器定义一致，否则可能读到错误值，甚至触发异常。

### Tips

Linux 下若不支持 SSH 文件传输，可使用 `wget` 命令下载：

电脑端开启临时文件服务器：

```bash
python -m http.server 8888
```

将待传输文件放在启动命令时所在的目录下。

板卡端下载文件：

```bash
wget http://电脑ip:8888/file
```

---
