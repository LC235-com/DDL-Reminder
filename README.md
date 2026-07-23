# 智能日程与DDL提醒系统(DDL-reminder)
## 1.0版本，基本实现了asr-llm-tts语音交互回路

PC + ESP32-S3 双端智能DDL提醒系统。电脑端负责信息爬取、AI处理和调度，ESP32-S3作为智能交互终端（显示、触控、语音、灯光提醒）。

## 系统架构

```
ESP32-S3 (终端)                     PC (服务器)
┌──────────────┐                   ┌────────────────────┐
│ LVGL UI      │                   │ WebSocket Server   │
│ 时钟 + DDL卡片│  ←──WebSocket──→  │ ┌────────────────┐ │
│ 人物表情(Emoji)│   JSON + PCM音频  │ │ ASR (FunASR)   │ │
│ LED灯带提醒   │                   │ │ LLM (GLM-4)    │ │
│ 触屏+编码器   │                   │ │ TTS (Edge-TTS) │ │
│ 麦克风+扬声器 │                   │ │ DDL引擎        │ │
└──────────────┘                   │ │ 爬虫(ZJU+PTA)  │ │
                                    │ └────────────────┘ │
                                    └────────────────────┘
```

## 快速开始(施工中)

### 1. 电脑端服务器

```bash
推荐python版本3.10或3.11
可用anaconda管理环境 conda create -n DDL-reminder python=3.10
# 安装依赖
cd server
pip install -r requirements.txt

# 安装 FunASR（本地免费ASR，推荐）
pip install funasr

# 安装 Edge-TTS（免费TTS）
pip install edge-tts pydub

# 配置API密钥
# 智谱AI (GLM-4-Flash, 免费额度)
export ZHIPU_API_KEY="your-api-key"

# 其他可选API
export DASHSCOPE_API_KEY="sk-xxx"     # 阿里云 DashScope
export DEEPSEEK_API_KEY="sk-xxx"      # DeepSeek
export OPENAI_API_KEY="sk-xxx"        # OpenAI

# 配置学在浙大爬虫（可选）
export ZJU_USER="你的学号"
export ZJU_PASS="你的密码"

# 配置PTA爬虫（可选）
export PTA_COOKIES="PTASession=xxx; JSESSIONID=xxx"

# 启动服务器
python server.py
```

服务器默认监听 `0.0.0.0:8888`。确保防火墙允许该端口。

### 2. ESP32-S3 终端

```bash
# 前置条件：安装 ESP-IDF v5.x
# https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/

cd esp32

# 修改 WiFi 和服务器地址
# 编辑 main/main.cpp 中的:
#   WIFI_SSID, WIFI_PASS, WS_URI

# 配置项目
idf.py set-target esp32s3
idf.py menuconfig  # 根据需要调整显示配置

# 编译
idf.py build

# 烧录
idf.py -p COMx flash monitor
```

### 3. 硬件连接（与原理图一致）

| 外设 | 信号 | GPIO |
|------|------|------|
| **INMP441** 麦克风 | WS | 4 |
| | SCK | 5 |
| | SD | 6 |
| **MAX98357A** 扬声器 | DIN | 7 |
| | BCLK | 15 |
| | LRC | 16 |
| | SD (使能) | 8 |
| **LCD** SPI | SDA/MOSI | 3 |
| | CLK/SCK | 46 |
| | CS | 9 |
| | DC | 12 |
| | RST | 10 |
| | BL/PWM | 11 |
| **触摸屏** I2C | SDA | 21 |
| | SCL | 45 |
| | INT | 14 |
| | RST | 13 |
| **SK6812** LED灯带 | DIN | 41 |
| | DOUT | 悬空 |
| **EC11** 编码器 | A相 | 38 |
| | B相 | 39 |
| | 按键 | 40 |

## AI语音流水线

受 [小智AI](https://github.com/78/xiaozhi-esp32) 启发，采用 Provider 模式：

```
ESP32录音 → PCM 16kHz → WebSocket → 服务器
                                         │
                                    ┌────┴────┐
                                    │  VAD    │ (SileroVAD)
                                    │  ASR    │ (FunASR SenseVoiceSmall, 免费本地)
                                    │  LLM    │ (GLM-4-Flash, 免费云端)
                                    │  TTS    │ (Edge-TTS, 免费云端)
                                    └────┬────┘
                                         │
服务器 ← PCM 16kHz ← WebSocket ← 音频响应
```

所有模块可通过 `server/config.py` 切换：

| 模块 | 免费方案(默认) | 付费方案 |
|------|--------------|---------|
| ASR | FunASR SenseVoiceSmall | DashScope Paraformer, Whisper |
| LLM | GLM-4-Flash (智谱) | Qwen, DeepSeek, GPT-4o |
| TTS | Edge-TTS (微软) | DashScope CosyVoice |

## 通信协议

### 服务器 → ESP32

| 命令 | 说明 |
|------|------|
| `sync` | 全量数据同步 |
| `new_event` | 新增DDL事件 |
| `delete_event` | 删除DDL事件 |
| `remind` | 立即提醒 |
| `speak` | TTS语音+文字+表情 |
| `emotion` | 控制人物表情 |
| `led` | 控制LED灯带 |
| `pong` | 心跳回应 |

### ESP32 → 服务器

| 命令 | 说明 |
|------|------|
| `audio_start` / 音频数据 / `audio_end` | 语音输入 |
| `query` | 文本查询 |
| `event_action` | done / snooze 操作 |
| `request_sync` | 请求全量同步 |
| `ping` | 心跳 |

## 数据格式

```json
{
  "id": "uuid",
  "title": "高数作业",
  "course": "高等数学",
  "type": "作业",
  "source": "zju",
  "deadline": "2026-07-25T23:59:00+08:00",
  "advance_minutes": 1440,
  "url": "https://...",
  "status": "pending",
  "tag": "🔥"
}
```

## 扩展新数据源

在 `server/ddl/crawler.py` 中添加新的爬虫类：

```python
class MyCrawler:
    async def fetch(self) -> list[DDLItem]:
        # 实现你的爬虫逻辑
        # 返回 DDLItem 列表
        ...
```

然后在 `CrawlerScheduler.crawl_once()` 中调用。

## ESP32 UI 功能

- **主界面**: 时钟、日期、2个最近DDL卡片、录音按钮、人物表情
- **列表界面**: 可滚动DDL列表，触屏+编码器导航
- **详情界面**: 完整事件信息、倒计时、完成/推迟按钮
- **提醒弹窗**: 全屏提醒、LED红灯闪烁、语音播报
- **对话人物**: 根据AI情绪切换表情(😊🤔😲😟🗣️)

## 项目结构

```
DDL-reminder/
├── server/                  # PC端 Python 服务器
│   ├── server.py            # 主入口
│   ├── config.py            # 配置
│   ├── protocol.py          # 通信协议
│   ├── system_prompt.md     # LLM系统提示
│   ├── asr/                 # ASR模块
│   ├── llm/                 # LLM模块
│   ├── tts/                 # TTS模块
│   └── ddl/                 # DDL引擎
│       ├── models.py        # 数据模型
│       ├── store.py         # JSON持久化
│       ├── scheduler.py     # 提醒调度
│       └── crawler.py       # 统一爬虫
├── esp32/                   # ESP32-S3 固件
│   └── main/
│       ├── main.cpp         # 主程序
│       ├── protocol.h/cpp   # 协议解析
│       ├── led_strip.h/cpp  # LED控制
│       ├── encoder.h/cpp    # 编码器
│       └── ui/              # LVGL界面
└── README.md
```

## 参考项目

- 小智AI: https://github.com/78/xiaozhi-esp32
- 小智AI Python Server: https://github.com/xinnan-tech/xiaozhi-esp32-server
- Celechron/ZJU DDL 统一爬虫: zju-ddl-killer/
- 语音命令识别参考: speech_commands_recognition_with_llm/
