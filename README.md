# DEV-TOOLS-SUITE

SylixOS 开发工具集

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
