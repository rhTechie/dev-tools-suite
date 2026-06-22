#!/usr/bin/env python3
"""
SylixOS FTP Upload Tool
手动上传文件到 SylixOS 板卡的命令行工具
"""

import argparse
import os
import posixpath
import re
import sys
from ftplib import FTP
import xml.etree.ElementTree as ET

def normalize_remote_path(path):
    """标准化远端 POSIX 路径"""
    normalized = (path or '/').replace('\\', '/')
    normalized = posixpath.normpath(normalized)
    return '/' if normalized == '.' else normalized

def join_remote_path(base, *parts):
    """拼接远端 POSIX 路径"""
    current = normalize_remote_path(base)
    for part in parts:
        current = posixpath.join(current, str(part).replace('\\', '/'))
    return normalize_remote_path(current)

def split_remote_path(path):
    """拆分远端文件路径"""
    normalized = normalize_remote_path(path)
    remote_dir = posixpath.dirname(normalized)
    remote_file = posixpath.basename(normalized)
    return ('/' if remote_dir == '.' else remote_dir), remote_file

def format_size(size):
    """格式化文件大小输出"""
    return f"{size/1024:.1f}KB" if size < 1024 * 1024 else f"{size/1024/1024:.1f}MB"

def validate_permission_mode(mode):
    """校验 chmod 权限格式"""
    normalized = mode.strip()
    if not re.fullmatch(r'[0-7]{3,4}', normalized):
        raise argparse.ArgumentTypeError("权限格式必须是 3 或 4 位八进制，例如 755 或 0755")
    return normalized[-3:]

def send_ftp_command(ftp, commands):
    """按顺序尝试发送 FTP 命令，直到成功"""
    last_error = None

    for command in commands:
        try:
            return command, ftp.sendcmd(command)
        except Exception as exc:
            last_error = exc

    if last_error is None:
        raise RuntimeError("没有可执行的 FTP 命令")
    raise last_error

def set_remote_permissions(ftp, remote_path, mode):
    """上传完成后设置远端权限"""
    normalized_path = normalize_remote_path(remote_path)
    send_ftp_command(ftp, [
        f'SITE CHMOD {mode} {normalized_path}',
        f'SITE chmod {mode} {normalized_path}',
    ])
    print(f"  权限已设置: {normalized_path} -> {mode}")

def sync_remote(ftp):
    """执行远端 sync 落盘"""
    command, response = send_ftp_command(ftp, [
        'SITE SYNC',
        'SITE sync',
        'SYNC',
        'sync',
    ])
    print(f"执行落盘命令: {command} -> {response}")

def ensure_dir(ftp, path, chmod_mode=None):
    """确保目录存在，如果不存在则创建"""
    path = normalize_remote_path(path)
    dirs = []
    while path and path != '/':
        dirs.append(path)
        path = posixpath.dirname(path)

    dirs.reverse()
    for d in dirs:
        try:
            ftp.cwd(d)
        except Exception:
            try:
                parent = posixpath.dirname(d)
                if parent and parent != '/':
                    ftp.cwd(parent)
                ftp.mkd(d)
                print(f"  创建目录: {d}")
                if chmod_mode:
                    set_remote_permissions(ftp, d, chmod_mode)
            except Exception:
                raise

def upload_file(ftp, local_file, remote_path, chmod_mode=None):
    """上传单个文件"""
    remote_dir, remote_file = split_remote_path(remote_path)
    remote_path = join_remote_path(remote_dir, remote_file)

    # 确保目标目录存在
    ensure_dir(ftp, remote_dir, chmod_mode=chmod_mode)
    ftp.cwd(remote_dir)

    # 上传文件
    with open(local_file, 'rb') as f:
        ftp.storbinary(f'STOR {remote_file}', f)

    if chmod_mode:
        set_remote_permissions(ftp, remote_path, chmod_mode)

    size = os.path.getsize(local_file)
    size_str = format_size(size)
    print(f"✓ 上传成功: {remote_path} ({size_str})")
    return 1

def upload_directory_contents(ftp, local_dir, remote_dir, chmod_mode=None):
    """上传目录中的第一层普通文件"""
    remote_dir = normalize_remote_path(remote_dir)
    ensure_dir(ftp, remote_dir, chmod_mode=chmod_mode)
    ftp.cwd(remote_dir)

    uploaded_files = 0
    skipped_items = []
    files = sorted(os.listdir(local_dir))

    for name in files:
        local_path = os.path.join(local_dir, name)
        if os.path.islink(local_path):
            print(f"  警告: 跳过符号链接: {local_path}")
            skipped_items.append(local_path)
            continue
        if not os.path.isfile(local_path):
            print(f"  警告: 跳过非普通文件: {local_path}")
            skipped_items.append(local_path)
            continue

        uploaded_files += upload_file(
            ftp,
            local_path,
            join_remote_path(remote_dir, name),
            chmod_mode=chmod_mode,
        )

    print(f"  ✓ 目录上传成功: {remote_dir}/ ({uploaded_files} 个文件)")
    return uploaded_files, skipped_items

def upload_directory_tree(ftp, local_root, remote_root='/', chmod_mode=None):
    """递归上传目录树，适用于 rootfs 目录"""
    local_root = os.path.abspath(local_root)
    remote_root = normalize_remote_path(remote_root)

    if not os.path.isdir(local_root):
        raise RuntimeError(f"目录不存在: {local_root}")

    uploaded_files = 0
    skipped_items = []

    ensure_dir(ftp, remote_root, chmod_mode=chmod_mode)

    for current_root, dirnames, filenames in os.walk(local_root, topdown=True, followlinks=False):
        dirnames.sort()
        filenames.sort()

        relative_dir = os.path.relpath(current_root, local_root)
        remote_dir = remote_root if relative_dir == '.' else join_remote_path(remote_root, relative_dir)
        ensure_dir(ftp, remote_dir, chmod_mode=chmod_mode)

        kept_dirnames = []
        for dirname in dirnames:
            local_dir = os.path.join(current_root, dirname)
            if os.path.islink(local_dir):
                print(f"  警告: 跳过目录符号链接: {local_dir}")
                skipped_items.append(local_dir)
                continue
            kept_dirnames.append(dirname)
        dirnames[:] = kept_dirnames

        for filename in filenames:
            local_path = os.path.join(current_root, filename)
            if os.path.islink(local_path):
                print(f"  警告: 跳过文件符号链接: {local_path}")
                skipped_items.append(local_path)
                continue
            if not os.path.isfile(local_path):
                print(f"  警告: 跳过非普通文件: {local_path}")
                skipped_items.append(local_path)
                continue

            remote_path = join_remote_path(remote_dir, filename)
            uploaded_files += upload_file(ftp, local_path, remote_path, chmod_mode=chmod_mode)

    print(f"✓ rootfs 目录上传完成: {local_root} -> {remote_root} ({uploaded_files} 个文件)")
    if skipped_items:
        print(f"警告: 跳过 {len(skipped_items)} 个符号链接或特殊文件")

    return uploaded_files, skipped_items

def parse_config_mk(project_path):
    """从 config.mk 中提取平台列表和构建类型"""
    config_mk = os.path.join(project_path, 'config.mk')
    platforms = []
    debug_level = 'release'

    if not os.path.exists(config_mk):
        return platforms, debug_level

    with open(config_mk, 'r', encoding='utf-8', errors='ignore') as f:
        for line in f:
            stripped = line.strip()

            debug_match = re.match(r'^DEBUG_LEVEL\s*:?=\s*(.+)$', stripped)
            if debug_match:
                value = debug_match.group(1).strip().lower()
                if value in ('debug', 'release'):
                    debug_level = value
                continue

            platform_match = re.match(r'^PLATFORMS\s*:?=\s*(.+)$', stripped)
            if platform_match:
                value = platform_match.group(1).strip()
                if value:
                    platforms = value.split()

    return platforms, debug_level

def discover_build_platforms(project_path):
    """从 build 目录推断已存在的平台输出"""
    build_dir = os.path.join(project_path, 'build')
    if not os.path.isdir(build_dir):
        return []

    return sorted(
        entry for entry in os.listdir(build_dir)
        if os.path.isdir(os.path.join(build_dir, entry))
    )

def resolve_reproject_src_candidates(src, project_path, project_name, platforms, debug_level):
    """展开 .reproject 中的本地源路径变量，返回候选路径列表"""
    normalized = src.replace('\\', '/')
    normalized = normalized.replace(f'$(WORKSPACE_{project_name})', project_path)

    output_name = 'Debug' if debug_level == 'debug' else 'Release'

    replacements_list = []
    for platform in platforms:
        replacements_list.append({
            '$(Output)': f'build/{platform}/{output_name}',
            '$(Debug)': f'build/{platform}/Debug',
            '$(Release)': f'build/{platform}/Release',
        })

    # 兼容部分旧工程直接把 $(Output) 指向项目根下的 Release/Debug
    replacements_list.append({
        '$(Output)': output_name,
        '$(Debug)': 'Debug',
        '$(Release)': 'Release',
    })

    candidates = []
    seen = set()

    for replacements in replacements_list:
        resolved = normalized
        for token, value in replacements.items():
            resolved = resolved.replace(token, value)

        resolved = os.path.normpath(resolved)
        if resolved not in seen:
            seen.add(resolved)
            candidates.append(resolved)

    if not candidates:
        candidates.append(os.path.normpath(normalized))

    return candidates

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

    board_ip = (device_setting.get('DevName') or '').strip()
    if not board_ip:
        print("错误: .reproject 文件中找不到板卡 IP (DevName)")
        sys.exit(1)

    # 获取项目名称
    project_name = os.path.basename(project_path)
    config_platforms, debug_level = parse_config_mk(project_path)
    device_platform = (device_setting.get('Platform') or '').strip()
    build_platforms = discover_build_platforms(project_path)

    platforms = []
    if device_platform:
        platforms.append(device_platform)
    platforms.extend(config_platforms)
    platforms.extend(build_platforms)

    # 去重并保留顺序；完全没有线索时仍保留 ARM64_GENERIC 作为最后兜底
    ordered_platforms = []
    seen_platforms = set()
    for platform in platforms + ['ARM64_GENERIC']:
        if platform and platform not in seen_platforms:
            seen_platforms.add(platform)
            ordered_platforms.append(platform)

    # 解析上传路径
    upload_paths = []
    for pair in root.findall('.//UploadPath/PairItem'):
        src = pair.get('key')
        dst = pair.get('value')

        if not src or not dst:
            continue

        # 展开 RealEvo 导出的工作区与输出目录变量
        candidates = resolve_reproject_src_candidates(
            src,
            project_path,
            project_name,
            ordered_platforms,
            debug_level,
        )

        resolved_src = next((candidate for candidate in candidates if os.path.exists(candidate)), None)
        if resolved_src:
            upload_paths.append((resolved_src, dst))
        else:
            print(f"警告: 文件不存在，跳过: {candidates[0]}")

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

  # 递归上传 rootfs 到板卡根目录
  # 默认每个文件上传后 chmod 755，结束后执行一次 sync
  %(prog)s -i 10.13.21.42 --rootfs /path/to/rootfs --rootfs-target /
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
    parser.add_argument('-r', '--rootfs', help='本地 rootfs 目录路径（递归上传，无需解析 .reproject）')
    parser.add_argument('--rootfs-target', default='/', help='rootfs 上传目标根目录（默认: /）')
    parser.add_argument('-m', '--chmod', type=validate_permission_mode, default='755', help='上传成功后执行远端 chmod（默认: 755）')
    parser.add_argument('--no-chmod', action='store_true', help='禁用上传成功后的远端 chmod')
    parser.add_argument('--sync', dest='sync', action='store_true', default=True, help='所有上传完成后执行一次远端 sync（默认开启）')
    parser.add_argument('--no-sync', dest='sync', action='store_false', help='禁用所有上传完成后的远端 sync')

    args = parser.parse_args()

    # 检查参数
    selected_modes = sum(bool(value) for value in [args.project, args.config, args.file, args.rootfs])
    if selected_modes == 0:
        parser.error("必须指定 -P/--project、-f/--file、-c/--config 或 -r/--rootfs")
    if selected_modes > 1:
        parser.error("-P/--project、-f/--file、-c/--config 和 -r/--rootfs 只能选择一种模式")

    if args.file and not (args.target or args.dir):
        parser.error("使用 -f/--file 时必须指定 -t/--target 或 -d/--dir")
    if args.file and args.target and args.dir:
        parser.error("-t/--target 和 -d/--dir 只能选择一个")
    if args.rootfs_target != '/' and not args.rootfs:
        parser.error("--rootfs-target 只能和 -r/--rootfs 一起使用")

    chmod_mode = None if args.no_chmod else args.chmod

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

        if not upload_list:
            print("错误: 没有解析到任何有效上传项，请检查 .reproject 路径和构建产物是否存在")
            sys.exit(1)

    if not board_ip:
        parser.error("必须指定板卡 IP (-i/--ip) 或使用包含 IP 配置的项目 (-P/--project)")

    try:
        # 连接 FTP
        print(f"正在连接 {board_ip}...")
        ftp = FTP()
        ftp.connect(board_ip, 21, timeout=10)
        ftp.login(args.user, args.password)
        ftp.set_pasv(True)
        print(f"登录成功！\n")

        success_count = 0
        fail_count = 0
        uploaded_file_count = 0

        if args.project:
            # 从 .reproject 解析的上传列表
            for i, (src, dst) in enumerate(upload_list, 1):
                try:
                    print(f"[{i}/{len(upload_list)}] {os.path.basename(src)}")

                    if os.path.isfile(src):
                        uploaded_file_count += upload_file(ftp, src, dst, chmod_mode=chmod_mode)
                        success_count += 1
                    elif os.path.isdir(src):
                        uploaded_files, _ = upload_directory_contents(
                            ftp,
                            src,
                            dst,
                            chmod_mode=chmod_mode,
                        )
                        uploaded_file_count += uploaded_files
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
                        if os.path.isfile(local_file):
                            uploaded_file_count += upload_file(
                                ftp,
                                local_file,
                                remote_path,
                                chmod_mode=chmod_mode,
                            )
                        elif os.path.isdir(local_file):
                            uploaded_files, _ = upload_directory_tree(
                                ftp,
                                local_file,
                                remote_path,
                                chmod_mode=chmod_mode,
                            )
                            uploaded_file_count += uploaded_files
                        success_count += 1
                    else:
                        print(f"✗ 文件不存在: {local_file}")
                        fail_count += 1
        elif args.rootfs:
            print(f"上传 rootfs: {args.rootfs}")
            uploaded_files, _ = upload_directory_tree(
                ftp,
                args.rootfs,
                args.rootfs_target,
                chmod_mode=chmod_mode,
            )
            uploaded_file_count += uploaded_files
            success_count += 1
        else:
            # 上传单个文件
            if not os.path.exists(args.file):
                print(f"错误: 文件不存在: {args.file}")
                sys.exit(1)

            # 确定目标路径
            if args.target:
                remote_path = normalize_remote_path(args.target)
            else:
                remote_path = join_remote_path(args.dir, os.path.basename(args.file))

            print(f"上传: {os.path.basename(args.file)}")
            uploaded_file_count += upload_file(
                ftp,
                args.file,
                remote_path,
                chmod_mode=chmod_mode,
            )
            success_count += 1

        if args.sync:
            sync_remote(ftp)

        ftp.quit()

        print("\n=== 上传完成 ===")
        if args.project or args.config or args.rootfs:
            print(f"成功: {success_count}")
            print(f"失败: {fail_count}")
            print(f"总计: {success_count + fail_count}")
            print(f"实际上传文件数: {uploaded_file_count}")
        else:
            print("上传成功！")

    except Exception as e:
        print(f"\n错误: {e}")
        sys.exit(1)

if __name__ == '__main__':
    main()
