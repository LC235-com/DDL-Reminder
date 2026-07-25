# 智能日程与DDL提醒系统 (DDL-reminder)

## v2.0 — 智能语音管家，触屏交互终端

PC + ESP32-S3 双端智能DDL提醒系统。相比 v1.0 仅实现了基础 ASR-LLM-TTS 语音回路，v2.0 在以下方面取得重大进展：

### v2.0 核心成果

**🔊 语音交互全面升级**
- 语音添加、修改、删除、查询 DDL 全功能覆盖
- 修改/添加后 WebSocket 实时同步至 ESP32 列表（修复 sync 遗漏 bug）
- FunASR SenseVoiceSmall 格式 token 自动净化
- 录音防抖保护（800ms 冷却 + rec_busy 锁）
- 流式 ASR/TTS 文本实时显示在 LCD 语音栏

**📺 ESP32 触屏终端**
- **全新 LVGL UI 布局**：时钟/日期 → 表情 → 语音文字栏 → DDL 卡片(×2) → 标签栏 + 录音按钮
- 标签栏支持触屏点击切换页面（主页 / DDL列表 / 设置），编码器旋转高亮
- DDL 卡片点击直达详情页，倒计时精确到秒（天/小时/分钟格式）
- 详情页**双击字段可编辑**（标题/课程/截止时间/提前提醒），小键盘输入（内存优化中）
- CJK 中文字体正常渲染（14px 1bpp 思源字体，覆盖 CJK Unified Ideographs）

**🕷️ 教务网爬虫（基于 Celechron 架构）**
- 成功对接 ZJU CAS 统一身份认证（Playwright 自动 RSA 加密登录）
- **教务网**（zdbk.zju.edu.cn）：爬取期中/期末考试时间，含课程名、教室、座位号
- **学在浙大**（courses.zju.edu.cn）：爬取作业/待办 DDL
- 仅保留未来事件，过期自动过滤
- 数据纯本地存储，保障隐私安全
- 支持手动 DDL（manual_ddl.json）作为离线补充

**📊 服务端增强**
- **自动清理**：已完成/已过期超 3 天的 DDL 定期清理（3 天周期）
- **离线提醒补发**：ESP32 断电期间错过的提醒，重连后自动推送并语音播报
- **人性化时间显示**：`3d 5h 12m left` 替代原始的 `4000+ 分钟`
- **DDL 修改去重**：同标题+课程+截止时间的 DDL 自动合并，避免重复

**🔧 硬件适配**
- ST7789V 240×320 IPS LCD 正常显示（BGR 像素顺序，SPI Mode 0，20MHz）
- FT6336U 电容触摸控制器驱动（I2C 0x38，多地址回退探测）
- EC11 编码器导航（6 脉冲消抖，上下文感知旋转/按键）
- INMP441 MEMS 麦克风 + MAX98357A 扬声器（I2S 全双工）
- SK6812 LED 灯带状态指示（录音红/处理蓝/提醒闪烁/待机灭）

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

# 修改 main/main.cpp 中的 WiFi 和服务器地址
# WIFI_SSID, WIFI_PASS, WS_URI
注意：WS_URI 填写服务器地址，如：`ws://192.168.1.100:8888`

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
│       ├── ddl_store.json     # DDL 持久化存储
│       └── manual_ddl.json    # 手动 DDL 示例数据
├── esp32/                     # ESP32-S3 固件
│   └── main/
│       ├── main.cpp           # 主程序（LCD/触摸/编码器/WiFi/WS/音频）
│       ├── protocol.h/cpp     # WebSocket 协议解析
│       ├── touch_ft6336.h/cpp # FT6336U 触摸驱动
│       ├── remote_display.h/cpp # UDP 远程镜像（优化中）
│       ├── my_chinese_font.c  # CJK 中文字体（14px 1bpp 思源）
│       └── ui/
│           ├── ui_manager.h   # UI 管理器接口
│           └── ui_manager.cpp # LVGL 界面实现（主页/列表/详情/小键盘）
└── README.md
```

## v2.0 vs v1.0 对比

| 功能 | v1.0 | v2.0 |
|------|------|------|
| 语音回路 | ASR→LLM→TTS 基础打通 | 增删改查全覆盖 + 结果实时同步 |
| LCD 显示 | 240×240 颜色异常 | 240×320 色彩正常 + CJK 中文 |
| UI 布局 | 简单卡片 + Emoji | 5 层分区布局 + 标签栏触屏切换 |
| DDL 管理 | 仅查询 | 添加 / 修改 / 完成 / 推迟 / 触屏编辑 |
| 爬虫 | Playwright 基础版，未验证 | 对接教务网 + 学在浙大，通过验证 |
| 时间显示 | `4000 分钟` / `300 小时` | `2d 18h 40m left` |
| 数据清理 | 无 | 3 天自动清理过期 DDL |
| 离线提醒 | 无 | 断电错过提醒，开机自动补发 |
| 编码器 | 基础旋转 | 6 脉冲消抖 + 上下文感知 + 标签导航 |
| 触屏 | 不工作 | FT6336U 驱动就绪（待硬件上拉电阻） |

## 参考项目

- 小智AI: https://github.com/78/xiaozhi-esp32
- Celechron: https://github.com/lelexia0131/Celechron
- ZJU-DDL-Scraper: https://github.com/Evelina-IS/zju-ddl-killer/tree/main/ZJU-DDL-Scraper
- 语音命令识别: https://github.com/shenjingnan/xiaozhi-replica
- 小智硬件：https://oshwhub.com/chaeng/chaeng_xiaozhiaiv8