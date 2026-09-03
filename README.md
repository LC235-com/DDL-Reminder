# 智能日程与DDL提醒系统 (DDL-reminder)

## v3.0 — 稳定显示、完整交互与可靠语音控制

PC + ESP32-S3 双端智能 DDL 提醒系统。v3.0 聚焦长期运行稳定性、触屏/EC11 完整操作、可编辑多级提醒，以及“语音指令确实执行”的可靠性。

### v3.0 核心成果

**📺 稳定的 LVGL 触屏终端**
- 240×320 竖屏支持 180° 旋转，主页 / DDL 列表 / 设置均可触屏或 EC11 操作
- LVGL 更新统一回到 UI 任务并加锁，合并高频同步，降低白屏、看门狗和内存碎片风险
- DDL 列表支持完整滚动、高亮选择，并插入 1 天 / 7 天 / 1 个月时间节点
- 首页仅显示两个摘要；列表标题与正常问答保留滚动显示，长历史回复使用单行省略和详情页查看
- 扩充中文及常用符号字库，修复括号、罗马数字、日期分隔符等方框字符
- 黑金主题界面，麦克风和删除操作保留红色危险色

**🎛️ EC11 与提醒编辑**
- 标签栏旋转高亮、按键确认；列表旋转选中 DDL，详情页继续编辑
- 截止时间支持逐位循环修改
- 每个 DDL 最多 6 条提前提醒，按 30 天 / 24 小时 / 60 分钟编辑
- 提醒条目支持新增、选中修改和红色删除；重复提醒自动合并，零值单位自动隐藏
- 默认提醒：提前 1 天、3 小时、1 小时、30 分钟、10 分钟，以及截止当天 08:00

**🎙️ 可靠的语音增删改查**
- FunASR → LLM 工具调用 → EventStore → Edge-TTS 完整链路
- 修改、完成或删除必须以服务端真实工具结果为准，避免“嘴上成功、实际未执行”
- 课程名与标题联合匹配，兼容空格、口语连接词、标点和少量 ASR 近音错误
- 大模型未按要求返回工具调用时，为完成/删除操作提供确定性兜底
- 修改已有 DDL 时更新原条目；确实不存在且提供完整截止时间时才新建

**🔊 连续低延迟音频**
- 使用显式 `audio_stream_start` / `audio_stream_end` 边界，不再误用 WebSocket PING 结束 TTS
- 独立 FreeRTOS 播放任务和 32KB StreamBuffer，预缓存约 512ms 后连续写入 I2S
- 禁用 Wi-Fi modem sleep，降低无线调度造成的 PCM 欠载和断续
- 每次播放记录缓冲区 underrun 数，便于从串口日志直接定位音频问题

**⏰ 同步、清理与手机通知**
- 手动刷新显示成功/失败反馈，屏幕长按恢复时同步刷新 DDL
- 完成或过期记录保留 14 天后清理，清理任务每 7 天运行
- 历史对话保留最近 20 条，进入记录页自动定位到最新消息
- 可选 SMTP 邮件和钉钉机器人提醒；移动端通知与 ESP32 在线状态相互独立
- 设备重连后补发错过的提醒

---

## 系统架构

```
ESP32-S3 (终端)                     PC (服务器)
┌──────────────┐                   ┌────────────────────┐
│ LVGL UI      │                   │ WebSocket Server   │
│ 时钟 + DDL卡片│  ←──WebSocket──→  │ ┌────────────────┐ │
│ 语音文字栏    │   JSON + PCM音频  │ │ ASR (FunASR)   │ │
│ 表情 + 标签栏 │                   │ │ LLM (GLM-4)    │ │
│ LED灯带提醒   │                   │ │ TTS (Edge-TTS) │ │
│ 触屏+编码器   │                   │ │ DDL引擎        │ │
│ 麦克风+扬声器 │                   │ │ 爬虫(ZJU教务网)│ │
└──────────────┘                   │ └────────────────┘ │
                                    └────────────────────┘
```

## 快速开始

### 1. 电脑端服务器

```bash
# 推荐 Python 3.10~3.12 ，并用 Conda 管理虚拟环境：
conda create -n DDL-reminder python=3.10
conda activate DDL-reminder

cd server
pip install -r requirements.txt

# FunASR 本地免费 ASR
pip install funasr

# Edge-TTS 免费 TTS
pip install edge-tts

# Playwright（教务网爬虫登录用）
pip install playwright
playwright install chromium

# 配置 API 密钥
set ZHIPU_API_KEY=your-api-key        # 智谱AI GLM-4-Flash（免费额度）

# 配置学在浙大爬虫（可选）
set ZJU_USER=你的学号
set ZJU_PASS=你的密码

# 启动服务器
python server.py
```

服务器默认监听 `0.0.0.0:8888`。

**测试爬虫**（可选）：
```bash
python test_crawl.py        # 测试所有爬虫
python test_crawl.py zju    # 仅测试 ZJU 爬虫（教务网 + 学在浙大）
```

### 2. ESP32-S3 终端

```bash
# 前置条件：ESP-IDF v6.0.2
cd esp32

# 创建仅保存在本机的设备配置（不要提交 device_config.h）
copy main\device_config.example.h main\device_config.h
# 编辑 device_config.h 中的 WIFI_SSID、WIFI_PASS 和 WS_URI
# WS_URI 示例：ws://192.168.1.100:8888

idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

### 3. 硬件连接

| 外设 | 信号 | GPIO |
|------|------|------|
| **INMP441** 麦克风 | WS / SCK / SD | 4 / 5 / 6 |
| **MAX98357A** 扬声器 | DIN / BCLK / LRC / SD | 7 / 15 / 16 / 8 |
| **LCD** SPI | MOSI / CLK / CS / DC / RST / BL | 3 / 46 / 9 / 12 / 10 / 11 |
| **触摸屏** I2C | SDA / SCL / INT / RST | 21 / 45 / 14 / 13 |
| **SK6812** LED | DIN | 41 |
| **EC11** 编码器 | A / B / SW | 38 / 39 / 40 |

## AI 语音流水线

```
ESP32 录音 → PCM 16kHz → WebSocket → 服务器
                                         │
                                    ┌────┴────┐
                                    │  ASR    │ FunASR SenseVoiceSmall（免费本地）
                                    │  LLM    │ GLM-4-Flash（智谱，免费额度）
                                    │  TTS    │ Edge-TTS（微软，免费云端）
                                    └────┬────┘
                                         │
服务器 → PCM 16kHz → WebSocket → ESP32 播放
```

| 模块 | 免费方案(默认) | 可替换方案 |
|------|--------------|---------|
| ASR | FunASR SenseVoiceSmall | DashScope Paraformer, Whisper |
| LLM | GLM-4-Flash (智谱) | DeepSeek, Qwen, GPT-4o |
| TTS | Edge-TTS (微软) | DashScope CosyVoice |

## 通信协议

### 服务器 → ESP32

| 命令 | 说明 |
|------|------|
| `sync` | 全量数据同步（连接时 + DDL变更后） |
| `new_event` | 新增 DDL 事件 |
| `delete_event` | 删除 DDL 事件 |
| `remind` | 立即提醒 + 离线补发提醒 |
| `speak` | TTS 语音 + 文字 + 表情 |
| `audio_stream_start` / PCM音频 / `audio_stream_end` | 带唯一 stream_id 的 TTS 音频流 |
| `emotion` | 控制人物表情 |
| `asr_result` | ASR 识别结果（LCD 实时显示） |
| `led` | LED 灯带控制 |
| `pong` | 心跳回应 |

### ESP32 → 服务器

| 命令 | 说明 |
|------|------|
| `audio_start` / PCM音频 / `audio_end` | 语音输入 |
| `query` | 文本查询 |
| `event_action` | done / snooze / edit 操作 |
| `add_event` | 触屏手动创建 DDL |
| `request_sync` | 请求全量同步 |
| `ping` | 心跳 |

## 项目结构

```
DDL-reminder/
├── server/                    # PC 端 Python 服务器
│   ├── server.py              # 主入口（WebSocket + 清理任务 + 离线提醒）
│   ├── config.py              # 配置（AI模块/爬虫/清理周期）
│   ├── notifications.py       # 邮件 + 钉钉机器人通知
│   ├── protocol.py            # 通信协议定义
│   ├── system_prompt.md       # LLM 系统提示词
│   ├── test_crawl.py          # 爬虫测试脚本
│   ├── asr/                   # ASR 模块
│   │   └── funasr_asr.py      # FunASR（含 token 净化）
│   ├── llm/                   # LLM 模块
│   │   ├── openai_compatible.py  # OpenAI 兼容接口 + DDL 工具定义
│   │   └── intent_parser.py      # 工具调用解析（增删改查）
│   ├── tts/                   # TTS 模块
│   │   └── edge_tts.py        # Edge-TTS 流式合成
│   ├── ddl/                   # DDL 引擎
│   │   ├── models.py          # 数据模型（人性化时间显示）
│   │   ├── store.py           # JSON 持久化（清理/离线提醒查询）
│   │   ├── scheduler.py       # 定时提醒触发
│   │   └── crawler.py         # 统一爬虫（ZJU教务网 + 学在浙大 + PTA + Local）
│   └── data/
│       ├── ddl_store.json     # 本地 DDL 数据（运行时生成，不提交 Git）
│       └── manual_ddl.json    # 手动 DDL 示例数据
├── esp32/                     # ESP32-S3 固件
│   └── main/
│       ├── main.cpp           # 主程序（LCD/触摸/编码器/WiFi/WS/音频）
│       ├── protocol.h/cpp     # WebSocket 协议解析
│       ├── touch_ft6336.h/cpp # FT6336U 触摸驱动
│       ├── remote_display.h/cpp # UDP 远程镜像（优化中）
│       ├── common_symbols_font.c # 常用中文、标点及符号字库
│       ├── device_config.example.h # WiFi/WebSocket 配置模板
│       └── ui/
│           ├── ui_manager.h   # UI 管理器接口
│           └── ui_manager.cpp # LVGL 界面实现（主页/列表/详情/小键盘）
└── README.md
```

## v3.0 vs v2.0 对比

| 功能 | v2.0 | v3.0 |
|------|------|------|
| 语音工具 | 依赖模型正确调用 | 强制路由、跨字段匹配、真实结果确认 |
| TTS 传输 | PING 兼作结束标记 | 显式流边界 + 独立播放任务 + 预缓冲 |
| LCD 稳定性 | 长时间使用可能白屏 | UI 单线程更新、同步合并、内存监测与恢复 |
| DDL 列表 | 仅显示可见条目 | 全列表滚动 + 1天/7天/1个月节点 |
| 提醒设置 | 单一提前时间 | 最多 6 条，30天/24小时/60分钟逐位编辑 |
| 编码器 | 基础导航 | 标签、列表、详情、设置全过程上下文控制 |
| 刷新反馈 | 无明确结果 | 成功/失败弹窗及耗时日志 |
| 手机通知 | 无 | SMTP 邮件与钉钉机器人可选 |
| 数据维护 | 3 天清理 | 完成/过期保留 14 天，每 7 天清理 |

## 手机提醒（可选）

不设置以下环境变量时，手机通知保持关闭；ESP32 本地提醒不受影响。

```powershell
# SMTP 邮件（示例使用 SSL 465）
$env:DDL_NOTIFY_EMAIL_TO="you@example.com"
$env:DDL_SMTP_HOST="smtp.example.com"
$env:DDL_SMTP_PORT="465"
$env:DDL_SMTP_USER="sender@example.com"
$env:DDL_SMTP_PASSWORD="邮箱授权码"

# 钉钉自定义机器人
$env:DDL_DINGTALK_WEBHOOK="https://oapi.dingtalk.com/robot/send?access_token=..."
$env:DDL_DINGTALK_SECRET="SEC..."  # 机器人未启用加签时可省略
```

新建 DDL 默认在提前 1 天、3 小时、1 小时、30 分钟、10 分钟及截止当天 08:00 提醒。详情页触碰截止时间或提醒时间后，旋转 EC11 逐位修改，短按移动到下一位并在末位保存；把提醒改成 `0090` 会替换为仅提前 90 分钟提醒一次。

## 参考项目

- 小智AI: https://github.com/78/xiaozhi-esp32
- Celechron: https://github.com/lelexia0131/Celechron
- ZJU-DDL-Scraper: https://github.com/Evelina-IS/zju-ddl-killer/tree/main/ZJU-DDL-Scraper
- 语音命令识别: https://github.com/shenjingnan/xiaozhi-replica
- 小智硬件：https://oshwhub.com/chaeng/chaeng_xiaozhiaiv8
