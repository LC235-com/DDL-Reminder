#!/usr/bin/env python3
"""
DDL Reminder System — Main WebSocket Server.

Orchestrates all modules:
- ASR → LLM → TTS voice pipeline
- DDL engine (crawler + scheduler)
- WebSocket communication with ESP32-S3

Inspired by: 小智AI (xiaozhi-ai) provider pattern
Reference: speech_commands_recognition_with_llm/server/server.py

Usage:
    python server.py

Environment variables:
    ZHIPU_API_KEY        — GLM-4-Flash API key (free tier available)
    DASHSCOPE_API_KEY    — DashScope API key (for Paraformer ASR / CosyVoice TTS)
    DEEPSEEK_API_KEY     — DeepSeek API key
    OPENAI_API_KEY       — OpenAI API key (for Whisper ASR)
    ZJU_USER / ZJU_PASS  — 学在浙大 credentials
    PTA_COOKIES          — PTA cookie string
"""

import asyncio
import base64
import json
import logging
import os
import sys
import time
import wave
from datetime import datetime
from pathlib import Path

import websockets

# Add server directory to path
sys.path.insert(0, str(Path(__file__).parent))

from config import (
    WS_HOST, WS_PORT,
    ASR_PROVIDER, LLM_PROVIDER, TTS_PROVIDER,
    DASHSCOPE_API_KEY, EDGE_TTS_VOICE, COSYVOICE_VOICE,
    SAMPLE_RATE, CHANNELS, BIT_DEPTH,
    REMINDER_CHECK_INTERVAL, CRAWL_INTERVAL,
    DEFAULT_ADVANCE_MINUTES, EMOTION_ENABLED,
    get_llm_config, check_config,
)
from protocol import (
    msg_sync, msg_new_event, msg_delete_event, msg_remind,
    msg_speak, msg_emotion, msg_led, msg_pong,
)
from ddl.models import DDLItem
from ddl.store import EventStore
from ddl.scheduler import ReminderScheduler
from ddl.crawler import CrawlerScheduler

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger("DDL-Server")

# ── Global state ──────────────────────────────────────────────
store: EventStore = None
reminder_scheduler: ReminderScheduler = None
crawler_scheduler: CrawlerScheduler = None
asr_module = None
llm_module = None
tts_module = None
intent_parser = None

# Connected ESP32 clients: {websocket: client_state}
connected_clients: dict = {}


# ── AI Module Initialization ──────────────────────────────────

def init_asr():
    """Initialize ASR module based on config."""
    global asr_module
    from asr.base import BaseASR

    logger.info(f"Initializing ASR: {ASR_PROVIDER}")

    if ASR_PROVIDER == "funasr":
        from asr.funasr_asr import FunASR
        asr_module = FunASR()
    elif ASR_PROVIDER == "dashscope":
        from asr.dashscope_asr import DashScopeASR
        asr_module = DashScopeASR(DASHSCOPE_API_KEY)
    elif ASR_PROVIDER == "whisper":
        from asr.whisper_asr import WhisperASR
        asr_module = WhisperASR()
    else:
        logger.warning(f"Unknown ASR provider '{ASR_PROVIDER}', using FunASR")
        from asr.funasr_asr import FunASR
        asr_module = FunASR()


def init_llm():
    """Initialize LLM module based on config."""
    global llm_module
    from llm.openai_compatible import OpenAICompatibleLLM

    cfg = get_llm_config()
    logger.info(f"Initializing LLM: {LLM_PROVIDER} (model={cfg['model']})")
    llm_module = OpenAICompatibleLLM(cfg)


def init_tts():
    """Initialize TTS module based on config."""
    global tts_module
    logger.info(f"Initializing TTS: {TTS_PROVIDER}")

    if TTS_PROVIDER == "edge":
        from tts.edge_tts import EdgeTTS
        tts_module = EdgeTTS(EDGE_TTS_VOICE)
    elif TTS_PROVIDER == "dashscope":
        from tts.dashscope_tts import DashScopeTTS
        tts_module = DashScopeTTS(DASHSCOPE_API_KEY, COSYVOICE_VOICE)
    else:
        logger.warning(f"Unknown TTS provider '{TTS_PROVIDER}', using Edge-TTS")
        from tts.edge_tts import EdgeTTS
        tts_module = EdgeTTS(EDGE_TTS_VOICE)


# ── System Prompt ─────────────────────────────────────────────

async def load_system_prompt() -> str:
    """Load system prompt and populate with current DDL context."""
    prompt_path = Path(__file__).parent / "system_prompt.md"
    if prompt_path.exists():
        base = await asyncio.to_thread(prompt_path.read_text, encoding="utf-8")
    else:
        base = "你是一个智能DDL助手。"

    # Build events context (async — called from event loop, safe)
    events = await store.get_pending()
    if events:
        lines = ["当前待办事项："]
        for e in events[:20]:
            lines.append(
                f"  • [{e.tag()}] {e.course} — {e.title} "
                f"(截止: {e.deadline_str()}, 剩余: {e.minutes_remaining()}分钟)"
            )
        context = "\n".join(lines)
    else:
        context = "暂无待办事项。"

    return base.replace("{events_context}", context)


# ── Client State ──────────────────────────────────────────────

class ClientState:
    """Per-connection state."""
    def __init__(self):
        self.audio_buffer = bytearray()
        self.is_recording = False
        self.conversation_history: list[dict] = []

    def clear_audio(self):
        self.audio_buffer = bytearray()
        self.is_recording = False


# ── Reminder Handler ──────────────────────────────────────────

async def on_reminder_trigger(event: DDLItem):
    """Called by ReminderScheduler when a reminder is due."""
    logger.info(f"🔔 Reminder triggered: {event.title}")

    # Build reminder TTS text
    mins = event.minutes_remaining()
    if mins < 0:
        tts_text = f"提醒：{event.title}已经过期了！"
        emotion = "sad"
    elif mins < 60:
        tts_text = f"紧急提醒：{event.title}将在{mins}分钟后截止！"
        emotion = "surprised"
    else:
        hours = mins // 60
        tts_text = f"提醒：{event.title}将在{hours}小时后截止"
        emotion = "surprised"

    # Synthesize reminder audio
    audio = await tts_module.synthesize(tts_text) if tts_module else b""

    event_dict = event.to_dict()

    for ws, state in connected_clients.items():
        try:
            # Send remind command
            await ws.send(json.dumps(msg_remind(event_dict), ensure_ascii=False))

            # Also send audio if available (PCM binary chunks + ping to end)
            if audio:
                await ws.send(json.dumps(msg_speak("", tts_text, emotion), ensure_ascii=False))
                CHUNK = 4096
                for offset in range(0, len(audio), CHUNK):
                    await ws.send(audio[offset:offset + CHUNK])
                await ws.ping()

            # Flash LED red
            await ws.send(json.dumps(msg_led("flash", "#FF0000"), ensure_ascii=False))
        except Exception as e:
            logger.error(f"Failed to notify client: {e}")


# ── Voice Pipeline ────────────────────────────────────────────

async def process_voice_query(websocket, state: ClientState) -> None:
    """Run the ASR → LLM → TTS pipeline for a voice query."""
    audio_data = bytes(state.audio_buffer)
    logger.info(f"Processing audio: {len(audio_data)} bytes ({len(audio_data)/2/SAMPLE_RATE:.1f}s)")

    if len(audio_data) < SAMPLE_RATE * 0.3 * 2:  # Less than 0.3 seconds
        logger.info("Audio too short, skipping")
        return

    # Step 1: ASR
    await websocket.send(json.dumps(msg_emotion("thinking"), ensure_ascii=False))
    logger.info(f"Calling ASR ({ASR_PROVIDER})...")

    transcript = ""
    if asr_module and await asr_module.is_available():
        transcript = await asr_module.transcribe(audio_data, SAMPLE_RATE)
        logger.info(f"ASR result: '{transcript}'")
    else:
        logger.warning("ASR not available")
        return

    if not transcript.strip():
        await websocket.send(json.dumps(msg_speak("", "抱歉，我没有听清楚，请再说一遍。", "sad"), ensure_ascii=False))
        return

    # Step 2: LLM — text understanding
    await websocket.send(json.dumps(msg_emotion("thinking"), ensure_ascii=False))

    # Build messages
    system_prompt = await load_system_prompt()
    messages = [
        {"role": "system", "content": system_prompt},
        *state.conversation_history[-6:],  # Last 6 messages for context
        {"role": "user", "content": transcript},
    ]

    from llm.openai_compatible import DDL_TOOLS

    result = {"response": "", "tool_calls": [], "emotion": "neutral"}
    if llm_module and await llm_module.is_available():
        result = await llm_module.chat(messages, DDL_TOOLS)
    else:
        result["response"] = "AI服务未配置，请检查API密钥。"

    # Step 3: Execute tool calls
    tool_result = ""
    if result["tool_calls"] and intent_parser:
        tool_result = await intent_parser.execute(result["tool_calls"])
        if tool_result:
            # Feed tool results back to LLM for final response
            messages.append({"role": "assistant", "content": None, "tool_calls": result["tool_calls"]})
            messages.append({"role": "tool", "content": tool_result, "tool_call_id": result["tool_calls"][0]["id"]})
            result = await llm_module.chat(messages, None)

    # Update conversation history
    state.conversation_history.append({"role": "user", "content": transcript})
    state.conversation_history.append({"role": "assistant", "content": result["response"]})
    if len(state.conversation_history) > 20:
        state.conversation_history = state.conversation_history[-20:]

    # Step 4: TTS — text to speech
    response_text = result["response"] or ""
    emotion = result.get("emotion", "neutral")

    await websocket.send(json.dumps(msg_emotion(emotion), ensure_ascii=False))

    audio = b""
    if response_text and tts_module and await tts_module.is_available():
        await websocket.send(json.dumps(msg_emotion("speaking"), ensure_ascii=False))
        audio = await tts_module.synthesize(response_text)

    # Step 5: Send response to ESP32
    # Protocol: JSON speak (text+emotion) → binary PCM chunks → ping (end marker)
    await websocket.send(json.dumps(
        msg_speak("", response_text, emotion),  # audio sent as binary, not base64
        ensure_ascii=False,
    ))

    if audio:
        # Send PCM audio as binary chunks (matching ESP32 streaming parser)
        CHUNK = 4096
        for offset in range(0, len(audio), CHUNK):
            await websocket.send(audio[offset:offset + CHUNK])

        # Send WebSocket ping frame to signal end of audio stream
        # (ESP32 uses PING opcode to trigger finishStreamingPlayback)
        await websocket.ping()

    logger.info(f"Voice pipeline complete: '{transcript}' → '{response_text[:50]}...'")


# ── WebSocket Handler ─────────────────────────────────────────

async def handle_client(websocket, path=None):
    """Handle a single ESP32 WebSocket connection."""
    client_ip = websocket.remote_address[0] if websocket.remote_address else "unknown"
    logger.info(f"🔗 Client connected: {client_ip}")

    state = ClientState()
    connected_clients[websocket] = state

    try:
        # Send full sync on connect
        events = await store.get_pending()
        sync_data = [e.to_dict() for e in events]
        await websocket.send(json.dumps(msg_sync(sync_data), ensure_ascii=False))
        logger.info(f"Sent sync: {len(sync_data)} events")

        # Message loop
        async for message in websocket:
            try:
                if isinstance(message, bytes):
                    # Binary audio data — always accept (may arrive before audio_start on reconnect)
                    state.audio_buffer.extend(message)
                    continue

                # JSON text message
                data = json.loads(message)
                cmd = data.get("cmd", "")

                if cmd == "audio_start":
                    state.is_recording = True
                    state.clear_audio()
                    logger.info(f"🎤 [{client_ip}] Recording started")

                elif cmd == "audio_end":
                    state.is_recording = False
                    logger.info(f"✅ [{client_ip}] Recording ended ({len(state.audio_buffer)} bytes)")

                    # Process voice query
                    await process_voice_query(websocket, state)
                    state.clear_audio()

                elif cmd == "query":
                    # Direct text query (no ASR needed)
                    text = data.get("text", "")
                    logger.info(f"💬 [{client_ip}] Query: '{text}'")

                    state.conversation_history.append({"role": "user", "content": text})

                    system_prompt = await load_system_prompt()
                    messages = [
                        {"role": "system", "content": system_prompt},
                        *state.conversation_history[-6:],
                    ]

                    from llm.openai_compatible import DDL_TOOLS
                    result = await llm_module.chat(messages, DDL_TOOLS) if llm_module else {"response": "", "tool_calls": [], "emotion": "neutral"}

                    # Execute tool calls
                    if result.get("tool_calls") and intent_parser:
                        tool_result = await intent_parser.execute(result["tool_calls"])
                        if tool_result:
                            messages.append({"role": "assistant", "content": None, "tool_calls": result["tool_calls"]})
                            messages.append({"role": "tool", "content": tool_result})
                            result = await llm_module.chat(messages, None)

                    state.conversation_history.append({"role": "assistant", "content": result.get("response", "")})

                    response_text = result.get("response", "")
                    emotion = result.get("emotion", "neutral")

                    await websocket.send(json.dumps(msg_emotion(emotion), ensure_ascii=False))

                    # TTS
                    audio = b""
                    if response_text and tts_module:
                        audio = await tts_module.synthesize(response_text)

                    # Send speak command (text + emotion) then binary PCM audio chunks
                    await websocket.send(json.dumps(msg_speak("", response_text, emotion), ensure_ascii=False))

                    if audio:
                        CHUNK = 4096
                        for offset in range(0, len(audio), CHUNK):
                            await websocket.send(audio[offset:offset + CHUNK])
                        await websocket.ping()

                elif cmd == "event_action":
                    event_id, action = data.get("id", ""), data.get("action", "")
                    if action == "done":
                        await store.update_status(event_id, "done")
                        logger.info(f"✅ [{client_ip}] Marked done: {event_id}")
                    elif action == "snooze":
                        await store.update_status(event_id, "snoozed")
                        logger.info(f"⏰ [{client_ip}] Snoozed: {event_id}")

                elif cmd == "request_sync":
                    events = await store.get_pending()
                    sync_data = [e.to_dict() for e in events]
                    await websocket.send(json.dumps(msg_sync(sync_data), ensure_ascii=False))

                elif cmd == "ping":
                    await websocket.send(json.dumps(msg_pong(), ensure_ascii=False))

            except json.JSONDecodeError:
                logger.warning(f"Invalid JSON from {client_ip}")
            except Exception as e:
                logger.error(f"Message handling error [{client_ip}]: {e}")
                import traceback
                traceback.print_exc()

    except websockets.exceptions.ConnectionClosed:
        logger.info(f"🔌 Client disconnected: {client_ip}")
    finally:
        if websocket in connected_clients:
            del connected_clients[websocket]


# ── Main ──────────────────────────────────────────────────────

async def main():
    """Server entry point."""
    global store, reminder_scheduler, crawler_scheduler, intent_parser

    print("=" * 60)
    print("DDL Reminder Server — 智能日程与DDL提醒系统")
    print("=" * 60)

    # Check config
    warnings = check_config()
    if warnings:
        for w in warnings:
            print(f"⚠️  {w}")
        print()

    # Initialize DDL store
    store = EventStore(os.path.join(os.path.dirname(__file__), "data", "ddl_store.json"))
    await store.load()
    events = await store.get_all()
    pending = await store.get_pending()
    print(f"📚 DDL Store: {len(events)} total, {len(pending)} pending")

    # Initialize AI modules
    init_asr()
    init_llm()
    init_tts()

    from llm.intent_parser import IntentParser
    intent_parser = IntentParser(store)

    # Check availability
    print(f"🎤 ASR: {ASR_PROVIDER} — {'✅' if (asr_module and await asr_module.is_available()) else '⚠️ not available'}")
    print(f"🧠 LLM: {LLM_PROVIDER} — {'✅' if (llm_module and await llm_module.is_available()) else '⚠️ not available'}")
    print(f"🔊 TTS: {TTS_PROVIDER} — {'✅' if (tts_module and await tts_module.is_available()) else '⚠️ not available'}")

    # Start reminder scheduler
    reminder_scheduler = ReminderScheduler(store, REMINDER_CHECK_INTERVAL)
    reminder_scheduler.on_remind(on_reminder_trigger)
    await reminder_scheduler.start()

    # Start crawler scheduler (first crawl runs immediately in loop)
    crawler_scheduler = CrawlerScheduler(store, CRAWL_INTERVAL)

    async def on_new_crawled(events):
        for ws in connected_clients:
            for e in events:
                try:
                    await ws.send(json.dumps(msg_new_event(e.to_dict()), ensure_ascii=False))
                except Exception:
                    pass

    crawler_scheduler.on_new(on_new_crawled)
    await crawler_scheduler.start()

    # Start WebSocket server
    print(f"\n🌐 WebSocket server: ws://0.0.0.0:{WS_PORT}")
    print("   Waiting for ESP32 connections...\n")

    async with websockets.serve(handle_client, WS_HOST, WS_PORT):
        await asyncio.Future()  # Run forever


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n🛑 Server stopped by user")
