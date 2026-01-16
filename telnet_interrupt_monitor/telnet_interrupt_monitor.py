# 导入Windows弹窗所需的内置库
import ctypes

import telnetlib
import time
import re

def extract_irq_count(command_output, irq_name):
    pattern = rf"{re.escape(irq_name)}\s+.+?\s+\d+\s+(\d+)\s+\d+\s+\d+\s+\d+"
    match = re.search(pattern, command_output, re.DOTALL)
    
    if match:
        try:
            return int(match.group(1))
        except ValueError:
            return None
    else:
        return None

def auto_telnet_device(host, username, password, command, irq_name, execute_times=2, interval=1):
    tn = None
    irq_counts = []
    need_exit = False
    
    try:
        print(f"正在连接 Telnet 服务器 {host}...")
        tn = telnetlib.Telnet(host, port=23, timeout=30)
        print("Telnet 连接建立成功，开始进行账户认证...")
        
        tn.read_until(b"login: ", timeout=10)  
        tn.write((username + "\n").encode('utf-8'))
        print(f"已发送用户名：{username}")
        
        tn.read_until(b"password: ", timeout=10)  
        tn.write((password + "\n").encode('utf-8'))
        print(f"已发送密码：{'*' * len(password)}")
        
        time.sleep(1)
        tn.read_very_eager()
        
        for i in range(execute_times):
            print(f"\n正在执行第 {i+1} 次命令: {command}")
            
            tn.write((command + "\n").encode('utf-8'))
            time.sleep(0.5)
            command_output = tn.read_very_eager().decode('utf-8', errors='ignore')
            
            irq_count = extract_irq_count(command_output, irq_name)
            irq_counts.append(irq_count)
            if irq_count:
                print(f"第 {i+1} 次提取到{irq_name}中断次数：{irq_count}")
            else:
                print(f"第 {i+1} 次未提取到有效的{irq_name}中断次数")
            
            if i < execute_times - 1:
                print(f"等待 {interval} 秒后执行下一次命令...")
                time.sleep(interval)
        
        if len(irq_counts) == 2 and all(count is not None for count in irq_counts):
            first_count, second_count = irq_counts
            print("\n=== 开始比较两次{irq_name}中断次数 ===".format(irq_name=irq_name))
            if first_count == second_count:
                # 1. 终端打印警告
                warning_info = f"⚠️  警告！两次{irq_name}中断次数相同：第1次={first_count}，第2次={second_count}"
                print(warning_info)
                
                # 2. 强制全局置顶弹窗（所有软件之上，无遮挡）
                ctypes.windll.user32.MessageBoxW(
                    0,  # 无父窗口，确保全局生效
                    f"两次{irq_name}中断次数相同！\n第1次：{first_count}\n第2次：{second_count}",  # 弹窗内容
                    "Telnet中断测试异常警告",  # 弹窗标题
                    # 样式参数优化：系统模态 + 置顶 + 警告图标 + 确定按钮（强制无遮挡）
                    0x1000 | 0x40000 | 0x30 | 0x01  
                )
                
                need_exit = True
                print("⚠️  检测到异常，脚本将自动退出！")
        
        print("\n正在退出 Telnet 会话...")
        tn.write(b"quit\n")
        time.sleep(0.5)
        print("Telnet 会话已结束")
        
    except ConnectionRefusedError:
        print(f"错误：无法连接到 {host}:23，Telnet 服务被拒绝")
    except TimeoutError:
        print(f"错误：连接 {host} 或等待提示符超时（超过指定时间）")
    except Exception as e:
        print(f"错误：发生未知异常 - {str(e)}")
    finally:
        try:
            if tn:
                tn.close()
        except:
            pass
    
    return need_exit

if __name__ == "__main__":
    TARGET_HOST = "10.13.21.42"
    LOGIN_USERNAME = "root"
    LOGIN_PASSWORD = "root"
    EXECUTE_COMMAND = "ints"
    TARGET_IRQ_NAME = "uart2_isr"
    CYCLE_INTERVAL_MINUTES = 1
    CYCLE_INTERVAL_SECONDS = CYCLE_INTERVAL_MINUTES * 60
    
    print("=" * 60)
    print(f"Telnet 循环测试脚本已启动，每 {CYCLE_INTERVAL_MINUTES} 分钟执行一次")
    print(f"监控IRQ：{TARGET_IRQ_NAME}，按 Ctrl+C 可手动终止脚本运行")
    print(f"检测到两次中断次数相同时，将弹出全局置顶警告并自动退出脚本")
    print("=" * 60 + "\n")
    
    # 1. 初始化轮次计数器
    round_count = 0
    # 2. 记录脚本启动时间
    script_start_time = time.time()
    
    try:
        while True:
            # 每进入一次循环，轮次计数器加1
            round_count += 1
            current_time = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())
            
            # 3. 修改打印信息，显示当前轮次编号
            print(f"\n========== 第 {round_count} 轮测试开始（{current_time}）==========")
            
            need_exit_script = auto_telnet_device(
                host=TARGET_HOST,
                username=LOGIN_USERNAME,
                password=LOGIN_PASSWORD,
                command=EXECUTE_COMMAND,
                irq_name=TARGET_IRQ_NAME,
                execute_times=2,
                interval=1
            )
            
            if need_exit_script:
                break
            
            # 4. 修改本轮结束打印信息，显示当前轮次编号
            print(f"\n========== 第 {round_count} 轮测试结束，等待 {CYCLE_INTERVAL_MINUTES} 分钟后执行下一轮 ==========\n")
            time.sleep(CYCLE_INTERVAL_SECONDS)
    
    except KeyboardInterrupt:
        # 5. 手动终止时，显示最终轮次和总运行时长
        total_run_seconds = time.time() - script_start_time
        total_run_formatted = time.strftime("%H小时%M分钟%S秒", time.gmtime(total_run_seconds))
        print(f"\n\n脚本已被用户手动终止！")
        print(f"最终执行到第 {round_count} 轮，脚本总运行时长：{total_run_formatted}")
    else:
        # 6. 异常检测终止时，显示最终轮次和总运行时长
        total_run_seconds = time.time() - script_start_time
        total_run_formatted = time.strftime("%H小时%M分钟%S秒", time.gmtime(total_run_seconds))
        print("\n\n=============================================")
        print(f"脚本因检测到两次中断次数相同，已正常终止")
        print(f"最终执行到第 {round_count} 轮，脚本总运行时长：{total_run_formatted}")
        print("=============================================")