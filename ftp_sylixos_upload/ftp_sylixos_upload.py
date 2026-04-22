#!/usr/bin/env python3
"""
SylixOS FTP Upload Tool
手动上传文件到 SylixOS 板卡的命令行工具
"""

import argparse
import os
import sys
from ftplib import FTP
import xml.etree.ElementTree as ET

def ensure_dir(ftp, path):
    """确保目录存在，如果不存在则创建"""
    dirs = []
    while path and path != '/':
        dirs.append(path)
        path = os.path.dirname(path)

    dirs.reverse()
    for d in dirs:
        try:
            ftp.cwd(d)
        except:
            try:
                parent = os.path.dirname(d)
                if parent and parent != '/':
                    ftp.cwd(parent)
                ftp.mkd(d)
                print(f"  创建目录: {d}")
            except:
                pass

def upload_file(ftp, local_file, remote_path):
    """上传单个文件"""
    remote_dir = os.path.dirname(remote_path)
    remote_file = os.path.basename(remote_path)

    # 确保目标目录存在
    ensure_dir(ftp, remote_dir)
    ftp.cwd(remote_dir)

    # 上传文件
    with open(local_file, 'rb') as f:
        ftp.storbinary(f'STOR {remote_file}', f)

    size = os.path.getsize(local_file)
    size_str = f"{size/1024:.1f}KB" if size < 1024*1024 else f"{size/1024/1024:.1f}MB"
    print(f"✓ 上传成功: {remote_path} ({size_str})")

def parse_reproject(project_path):
    """解析 .reproject 文件，返回板卡 IP 和上传列表"""
    reproject_file = os.path.join(project_path, '.reproject')

    if not os.path.exists(reproject_file):
        print(f"错误: 找不到 .reproject 文件: {reproject_file}")
        sys.exit(1)

    # 读取 GB2312 编码的 XML
    with open(reproject_file, 'r', encoding='gb2312') as f:
        content = f.read()

    root = ET.fromstring(content)

    # 获取板卡 IP
    device_setting = root.find('.//DeviceSetting')
    if device_setting is None:
        print("错误: .reproject 文件中找不到 DeviceSetting")
        sys.exit(1)

    board_ip = device_setting.get('DevName')
    if not board_ip:
        print("错误: .reproject 文件中找不到板卡 IP (DevName)")
        sys.exit(1)

    # 获取项目名称
    project_name = os.path.basename(project_path)

    # 解析上传路径
    upload_paths = []
    for pair in root.findall('.//UploadPath/PairItem'):
        src = pair.get('key')
        dst = pair.get('value')

        # 替换工作区变量
        src = src.replace(f'$(WORKSPACE_{project_name})', project_path)

        if os.path.exists(src):
            upload_paths.append((src, dst))
        else:
            print(f"警告: 文件不存在，跳过: {src}")

    return board_ip, upload_paths

def main():
    parser = argparse.ArgumentParser(
        description='SylixOS FTP 上传工具',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='''
示例:
  # 自动解析 .reproject 并上传（推荐）
  %(prog)s -P /path/to/project

  # 使用当前目录的 .reproject
  %(prog)s -P .

  # 指定板卡 IP（覆盖 .reproject 中的配置）
  %(prog)s -P . -i 10.13.21.100

  # 上传单个文件
  %(prog)s -i 10.13.21.42 -f lyn_drv.ko -t /lib/modules/drivers/lyn_drv.ko

  # 上传到指定目录（保持文件名）
  %(prog)s -i 10.13.21.42 -f liblyn_drv.so -d /lib/

  # 使用自定义凭证
  %(prog)s -i 10.13.21.42 -u admin -p admin123 -f test.ko -t /lib/modules/test.ko

  # 批量上传（使用配置文件）
  %(prog)s -i 10.13.21.42 -c upload_list.txt
        '''
    )

    parser.add_argument('-P', '--project', help='项目目录路径（自动解析 .reproject）')
    parser.add_argument('-i', '--ip', help='板卡 IP 地址（可选，覆盖 .reproject 配置）')
    parser.add_argument('-u', '--user', default='root', help='FTP 用户名 (默认: root)')
    parser.add_argument('-p', '--password', default='root', help='FTP 密码 (默认: root)')
    parser.add_argument('-f', '--file', help='本地文件路径')
    parser.add_argument('-t', '--target', help='目标文件路径（完整路径）')
    parser.add_argument('-d', '--dir', help='目标目录（保持原文件名）')
    parser.add_argument('-c', '--config', help='配置文件（每行格式: 本地路径|目标路径）')

    args = parser.parse_args()

    # 检查参数
    if not args.project and not args.config and not args.file:
        parser.error("必须指定 -P/--project、-f/--file 或 -c/--config")

    if args.file and not (args.target or args.dir):
        parser.error("使用 -f/--file 时必须指定 -t/--target 或 -d/--dir")

    # 解析 .reproject 文件
    upload_list = []
    board_ip = args.ip

    if args.project:
        print(f"解析项目配置: {args.project}")
        project_path = os.path.abspath(args.project)
        parsed_ip, parsed_list = parse_reproject(project_path)

        # 如果没有指定 IP，使用 .reproject 中的 IP
        if not board_ip:
            board_ip = parsed_ip

        upload_list = parsed_list
        print(f"板卡 IP: {board_ip}")
        print(f"找到 {len(upload_list)} 个上传项\n")

    if not board_ip:
        parser.error("必须指定板卡 IP (-i/--ip) 或使用包含 IP 配置的项目 (-P/--project)")

    try:
        # 连接 FTP
        print(f"正在连接 {board_ip}...")
        ftp = FTP()
        ftp.connect(board_ip, 21, timeout=10)
        ftp.login(args.user, args.password)
        print(f"登录成功！\n")

        success_count = 0
        fail_count = 0

        if args.project:
            # 从 .reproject 解析的上传列表
            for i, (src, dst) in enumerate(upload_list, 1):
                try:
                    print(f"[{i}/{len(upload_list)}] {os.path.basename(src)}")

                    if os.path.isfile(src):
                        upload_file(ftp, src, dst)
                        success_count += 1
                    elif os.path.isdir(src):
                        # 上传目录中的所有文件
                        ensure_dir(ftp, dst)
                        ftp.cwd(dst)

                        files = [f for f in os.listdir(src) if os.path.isfile(os.path.join(src, f))]
                        for filename in files:
                            src_file = os.path.join(src, filename)
                            with open(src_file, 'rb') as f:
                                ftp.storbinary(f'STOR {filename}', f)
                            size = os.path.getsize(src_file)
                            size_str = f"{size/1024:.1f}KB" if size < 1024*1024 else f"{size/1024/1024:.1f}MB"
                            print(f"  ✓ {filename} ({size_str})")

                        print(f"  ✓ 目录上传成功: {dst}/ ({len(files)} 个文件)")
                        success_count += 1

                    print()
                except Exception as e:
                    print(f"  ✗ 上传失败: {e}\n")
                    fail_count += 1

        elif args.config:
            # 从配置文件批量上传
            with open(args.config, 'r') as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith('#'):
                        continue

                    parts = line.split('|')
                    if len(parts) != 2:
                        print(f"跳过无效行: {line}")
                        continue

                    local_file, remote_path = parts
                    if os.path.exists(local_file):
                        print(f"上传: {os.path.basename(local_file)}")
                        upload_file(ftp, local_file, remote_path)
                        success_count += 1
                    else:
                        print(f"✗ 文件不存在: {local_file}")
                        fail_count += 1
        else:
            # 上传单个文件
            if not os.path.exists(args.file):
                print(f"错误: 文件不存在: {args.file}")
                sys.exit(1)

            # 确定目标路径
            if args.target:
                remote_path = args.target
            else:
                remote_path = os.path.join(args.dir, os.path.basename(args.file))

            print(f"上传: {os.path.basename(args.file)}")
            upload_file(ftp, args.file, remote_path)
            success_count += 1

        ftp.quit()

        print("\n=== 上传完成 ===")
        if args.project or args.config:
            print(f"成功: {success_count}")
            print(f"失败: {fail_count}")
            print(f"总计: {success_count + fail_count}")
        else:
            print("上传成功！")

    except Exception as e:
        print(f"\n错误: {e}")
        sys.exit(1)

if __name__ == '__main__':
    main()
