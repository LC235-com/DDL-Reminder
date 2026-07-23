"""
Server configuration — all settings in one place.
Copy this to config_local.py and customize for your environment.
"""

import os

# ── WebSocket Server ──────────────────────────────────────────
WS_HOST = os.environ.get("DDL_WS_HOST", "0.0.0.0")
WS_PORT = int(os.environ.get("DDL_WS_PORT", "8888"))

# ── ASR Module ────────────────────────────────────────────────
# Options: "funasr" (local, free), "dashscope" (cloud), "whisper" (cloud)
ASR_PROVIDER = os.environ.get("DDL_ASR_PROVIDER", "funasr")

# FunASR (local SenseVoiceSmall)
FUNASR_MODEL_DIR = os.environ.get("FUNASR_MODEL_DIR", "models/SenseVoiceSmall")

# DashScope (cloud)
DASHSCOPE_API_KEY = os.environ.get("DASHSCOPE_API_KEY", "")

# ── LLM Module ────────────────────────────────────────────────
# Options: "glm" (智谱), "deepseek", "qwen" (通义千问), "openai"
LLM_PROVIDER = os.environ.get("DDL_LLM_PROVIDER", "glm")

# Provider configs (OpenAI-compatible)
LLM_CONFIGS = {
    "glm": {
        "base_url": "https://open.bigmodel.cn/api/paas/v4/",
        "model": "glm-4-flash",
        "api_key": os.environ.get("ZHIPU_API_KEY", ""),
    },
    "deepseek": {
        "base_url": "https://api.deepseek.com/v1",
        "model": "deepseek-chat",
        "api_key": os.environ.get("DEEPSEEK_API_KEY", ""),
    },
    "qwen": {
        "base_url": "https://dashscope.aliyuncs.com/compatible-mode/v1",
        "model": "qwen-plus",
        "api_key": os.environ.get("DASHSCOPE_API_KEY", ""),
    },
    "openai": {
        "base_url": "https://api.openai.com/v1",
        "model": "gpt-4o-mini",
        "api_key": os.environ.get("OPENAI_API_KEY", ""),
    },
}

# ── TTS Module ────────────────────────────────────────────────
# Options: "edge" (free, Microsoft), "dashscope" (CosyVoice)
TTS_PROVIDER = os.environ.get("DDL_TTS_PROVIDER", "edge")

# Edge-TTS
EDGE_TTS_VOICE = os.environ.get("EDGE_TTS_VOICE", "zh-CN-XiaoxiaoNeural")

# CosyVoice (DashScope)
COSYVOICE_VOICE = os.environ.get("COSYVOICE_VOICE", "longxiaochun")

# ── Audio ─────────────────────────────────────────────────────
SAMPLE_RATE = 16000  # Hz, 16kHz for all audio
CHANNELS = 1         # Mono
BIT_DEPTH = 16       # 16-bit PCM

# ── DDL Engine ────────────────────────────────────────────────
DDL_STORE_PATH = os.path.join(os.path.dirname(__file__), "data", "ddl_store.json")
REMINDER_CHECK_INTERVAL = 30   # seconds between reminder checks
CRAWL_INTERVAL = 1800          # seconds (30 minutes) between crawls
DEFAULT_ADVANCE_MINUTES = 1440 # 24 hours default advance reminder

# ── Crawlers ──────────────────────────────────────────────────
ZJU_ENABLED = bool(os.environ.get("ZJU_USER", ""))
ZJU_USER = os.environ.get("ZJU_USER", "")
ZJU_PASS = os.environ.get("ZJU_PASS", "")

PTA_ENABLED = bool(os.environ.get("PTA_COOKIES", ""))
PTA_COOKIES = os.environ.get("PTA_COOKIES", "")

# ── Emotion detection ─────────────────────────────────────────
# LLM can output [emotion:xxx] tags; server parses and sends to ESP32
EMOTION_ENABLED = True
EMOTIONS = ["neutral", "happy", "thinking", "surprised", "sad", "speaking"]


def get_llm_config():
    """Get the current LLM provider's configuration."""
    return LLM_CONFIGS.get(LLM_PROVIDER, LLM_CONFIGS["glm"])


def check_config():
    """Check configuration and warn about missing keys."""
    warnings = []

    # ASR check
    if ASR_PROVIDER == "dashscope" and not DASHSCOPE_API_KEY:
        warnings.append("DASHSCOPE_API_KEY not set — cloud ASR will fail")

    # LLM check
    cfg = get_llm_config()
    if not cfg.get("api_key"):
        warnings.append(f"API key not set for LLM provider '{LLM_PROVIDER}'")

    return warnings
