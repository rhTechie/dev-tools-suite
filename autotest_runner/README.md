# autotest_runner

## 工具作用与使用场景

`autotest_runner` 是一个基于 Telnet 的自动化测试工具。

它适合这些场景：

- 需要通过 Telnet 登录板卡执行测试程序
- 需要循环执行某个测试项
- 需要根据输出内容自动判断测试是否通过
- 需要在失败时立即停止，并保留日志方便排查

这个工具的工作方式是：

- 启动时建立一次 Telnet 长连接
- 每轮按顺序执行指定测试项
- 如果全部通过，则进入下一轮
- 如果任意测试项失败，则立即停止

## 目录下各文件作用

- `autotest_runner.py`
  主脚本，负责 Telnet 连接、循环调度、日志和异常停止。

- `config.json`
  默认主配置文件，放公共配置，例如连接参数、循环参数、日志参数。

- `test_cases.json`
  默认测试用例文件，放具体测试项。

- `checks/interrupt_monitor.py`
  IRQ 中断计数检测脚本。

- `checks/keyword_check.py`
  关键字检测脚本，用于执行程序后根据输出关键字判断成功或失败。

## 如何使用

默认运行：

Linux：

```bash
python3 autotest_runner.py
```

Windows：

```bat
python autotest_runner.py
```

默认会读取：

- `config.json`
- `test_cases.json`

如果只想运行某一个测试项，可以直接写测试名称：

```bash
python3 autotest_runner.py interrupt_monitor
```

```bash
python3 autotest_runner.py keyword_check
```

如果想一轮里顺序运行多个测试项：

```bash
python3 autotest_runner.py interrupt_monitor keyword_check
```

它的执行逻辑是：

- 每一轮循环里
- 按你给出的顺序
- 把这些测试项依次执行一遍
- 只要其中任何一个失败，工具立即停止

如果不想用默认 `config.json`，也可以指定主配置文件：

```bash
python3 autotest_runner.py -c my_config.json
```

也可以同时指定测试名称：

```bash
python3 autotest_runner.py -c my_config.json keyword_check
```

## 当前支持的功能

当前支持这两类测试：

- `interrupt_monitor`
- `keyword_check`

### 1. interrupt_monitor

作用：

- 执行 `ints`
- 提取指定 IRQ 的中断计数
- 同一轮采样多次
- 如果计数不变化，则判定失败

当前 `test_cases.json` 中的默认配置是：

```json
{
  "name": "interrupt_monitor",
  "script": "checks/interrupt_monitor.py",
  "config": {
    "command": "ints",
    "irq_name": "uart2_isr",
    "capture_mode": "fixed_wait",
    "wait_seconds": 0.5,
    "samples": 2,
    "sample_interval_seconds": 1
  }
}
```

### 2. keyword_check

作用：

- 先执行一组可选的预处理命令
- 再执行主程序或主命令
- 根据输出中是否包含成功关键字、是否命中失败关键字来判断结果

当前 `test_cases.json` 中的默认配置是：

```json
{
  "name": "keyword_check",
  "script": "checks/keyword_check.py",
  "config": {
    "pre_commands": [
      "cd /apps/test"
    ],
    "command": "./test",
    "capture_mode": "until_prompt",
    "wait_seconds": 0.5,
    "timeout_seconds": 30,
    "required_keywords": [
      "RESULT: PASS"
    ],
    "forbidden_keywords": [
      "RESULT: FAIL",
      "Segmentation fault",
      "arguments error!",
      "parameter(s) error.",
      "panic",
      "Oops"
    ],
    "success_message": "test finished with RESULT: PASS"
  }
}
```

## 配置文件中各参数的作用

当前默认配置拆成两部分：

- `config.json`
- `test_cases.json`

### config.json

当前结构：

```json
{
  "connection": {},
  "runtime": {},
  "alert": {},
  "checks_file": "test_cases.json"
}
```

#### connection

用于描述 Telnet 连接和登录方式。

- `host`
  目标板卡 IP。

- `port`
  Telnet 端口，默认一般是 `23`。

- `username`
  登录用户名。

- `password`
  登录密码。

- `login_prompt`
  用户名提示符，例如 `login: `。

- `password_prompt`
  密码提示符，例如 `password: `。

- `shell_prompt`
  shell 提示符，用于 `until_prompt` 模式判断命令什么时候结束。

- `connect_timeout`
  建立 Telnet 连接的超时时间。

- `read_timeout`
  底层 socket 读超时时间。

- `command_timeout`
  命令执行的默认超时时间。

- `post_login_wait_seconds`
  发完密码后额外等待一小段时间，避免刚登录时 shell 还不稳定。

- `encoding`
  编码，通常保持 `utf-8`。

- `command_terminator`
  命令结束符，通常是 `\n`。

- `drain_wait_seconds`
  清理残留输出时的静默等待时间。

- `connect_retry_count`
  连接或登录失败后的重试次数。

- `connect_retry_interval_seconds`
  连接重试间隔。

#### runtime

用于描述循环测试行为。

- `round_interval_seconds`
  每轮测试结束后的等待时间。

- `max_rounds`
  最大轮数，`0` 表示无限循环。

#### alert

用于描述失败后的提醒和日志行为。

- `enable_windows_popup`
  Windows 下失败时是否弹窗。

- `title`
  弹窗标题。

- `log_dir`
  日志目录。

- `failure_output_dir`
  失败快照目录。

#### checks_file

- `checks_file`
  指定测试用例文件路径，当前默认是 `test_cases.json`。

### test_cases.json

用于定义具体测试项。

基本结构：

```json
[
  {
    "name": "case_name",
    "script": "checks/xxx.py",
    "config": {}
  }
]
```

字段说明：

- `name`
  测试项名称，建议唯一。运行时就是通过这个名称指定执行项。

- `script`
  检测脚本路径。

- `config`
  传给该检测脚本的参数。

- `enabled`
  可选，是否启用，默认启用。

## 哪些参数必须配置，哪些可以先不关注

### 必须关注的参数

这些通常是最需要你根据实际板卡修改的：

在 `config.json` 里：

- `connection.host`
- `connection.username`
- `connection.password`
- `connection.shell_prompt`

在 `test_cases.json` 里：

- `name`
- `script`
- `config`

如果是 `interrupt_monitor`，至少要关注：

- `command`
- `irq_name`

如果是 `keyword_check`，至少要关注：

- `command`
- `required_keywords`
- `forbidden_keywords`

### 可以暂时不用太关注的参数

这些一般先保持默认就够了：

- `port`
- `read_timeout`
- `post_login_wait_seconds`
- `encoding`
- `command_terminator`
- `drain_wait_seconds`
- `connect_retry_count`
- `connect_retry_interval_seconds`

只有在连接行为或输出读取不稳定时，才需要再细调。

## 新增测试时应该如何处理

如果后续要新增测试，不需要改主脚本，一般只需要做这两步：

### 情况 1：复用现有检测脚本

如果你的新测试本质上还是：

- IRQ 计数检测
  或者
- 关键字检测

那只需要在 `test_cases.json` 里新增一个测试项即可。

例如新增一个新的关键字检测：

```json
{
  "name": "my_test",
  "script": "checks/keyword_check.py",
  "config": {
    "pre_commands": [
      "cd /apps/mytest"
    ],
    "command": "./mytest",
    "capture_mode": "until_prompt",
    "timeout_seconds": 20,
    "required_keywords": [
      "PASS"
    ],
    "forbidden_keywords": [
      "FAIL",
      "panic"
    ]
  }
}
```

然后运行：

```bash
python3 autotest_runner.py my_test
```

### 情况 2：现有检测脚本不够用

如果你的测试判定逻辑比较特殊，就在 `checks/` 下新增一个新的检测脚本。

每个检测脚本都需要导出：

```python
def run(context, config):
    ...
```

最常用的接口有：

- `context.run_command(...)`
- `context.logger.log(...)`
- `context.logger.log_block(...)`
- `context.sleep(seconds)`
- `context.fail(message, details)`

写好后，再在 `test_cases.json` 里引用新的脚本即可。

## capture_mode 说明

当前支持两种输出读取方式：

- `until_prompt`
- `fixed_wait`

### until_prompt

含义：

- 发完命令后一直读输出
- 直到看到 `shell_prompt`
- 才认为命令执行结束

适合：

- 提示符稳定
- 命令执行完一定会回到 shell
- 希望尽量完整拿到整条输出

在这个模式下，`timeout_seconds` 是有效的，表示：

- 最多等待多少秒看到提示符
- 超时后判定失败

### fixed_wait

含义：

- 发完命令后固定等待 `wait_seconds`
- 然后直接收当前输出
- 不等待提示符

适合：

- 提示符不稳定
- 命令输出较短
- 不希望依赖提示符判断结束

### 推荐选择

- 跑完整测试程序，例如 `./test`
  建议 `until_prompt`

- 跑快速查看类命令，例如 `ints`
  建议 `fixed_wait`
