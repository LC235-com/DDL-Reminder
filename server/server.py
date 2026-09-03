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
from uuid import uuid4

import websockets

# Add server directory to path
sys.path.insert(0, str(Path(__file__).parent))

from config import (
    WS_HOST, WS_PORT,
    ASR_PROVIDER, LLM_PROVIDER, TTS_PROVIDER,
    DASHSCOPE_API_KEY, EDGE_TTS_VOICE, COSYVOICE_VOICE,
    SAMPLE_RATE, CHANNELS, BIT_DEPTH,
    REMINDER_CHECK_INTERVAL, CRAWL_INTERVAL,
    CLEANUP_INTERVAL, DEFAULT_ADVANCE_MINUTES, EXPIRED_CLEANUP_DAYS,
    EMOTION_ENABLED,
    get_llm_config, check_config,
)
from protocol import (
    msg_sync, msg_new_event, msg_delete_event, msg_remind,
    msg_speak, msg_audio_stream_start, msg_audio_stream_end,
    msg_emotion, msg_led, msg_pong, msg_asr_result, msg_tool_result,
)
from ddl.models import DDLItem
from ddl.store import EventStore
from ddl.scheduler import ReminderScheduler
from notifications import send_mobile_notification
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

    # Inject current date so LLM knows the correct year (not 2022)
    now = datetime.now().strftime("%Y年%m月%d日 %H:%M (周%w)")
    base = base.replace("{current_date}", f"现在是 {now}，所有DDL截止时间必须基于此日期。")
    return base.replace("{events_context}", context)


# ── Client State ──────────────────────────────────────────────

class ClientState:
    """Per-connection state."""
    def __init__(self):
        self.audio_buffer = bytearray()
        self.is_recording = False
        self.conversation_history: list[dict] = []
        # Prevent reminder and assistant audio streams from interleaving.
        self.speech_send_lock = asyncio.Lock()

    def clear_audio(self):
        self.audio_buffer = bytearray()
        self.is_recording = False


WRITE_TOOLS = {"add_reminder", "modify_reminder", "delete_reminder", "mark_done"}


async def _send_speech(websocket, state: ClientState, text: str, emotion: str, audio: bytes) -> None:
    """Send one framed PCM stream; WebSocket PING remains keepalive-only."""
    async with state.speech_send_lock:
        await websocket.send(json.dumps(msg_speak("", text, emotion), ensure_ascii=False))
        if not audio:
            return

        stream_id = uuid4().hex
        await websocket.send(json.dumps(
            msg_audio_stream_start(stream_id, SAMPLE_RATE), ensure_ascii=False
        ))
        chunk_size = 4096
        for offset in range(0, len(audio), chunk_size):
            await websocket.send(audio[offset:offset + chunk_size])
        await websocket.send(json.dumps(msg_audio_stream_end(stream_id), ensure_ascii=False))


def _mutation_tool_hint(text: str) -> str:
    """Return a strong explicit-action hint; semantic routing remains the LLM's job."""
    normalized = text.replace(" ", "")
    keyword_groups = (
        ("delete_reminder", ("删除", "删掉", "删了", "移除", "取消这个", "不要这个", "不需要了")),
        ("mark_done", ("已完成", "已经完成", "完成了", "已做完", "做完了", "交完", "交了", "提交了", "搞定了")),
        ("modify_reminder", ("修改", "更改", "变更", "改一下", "改下", "改到", "改成", "改为", "调整", "推迟", "延后", "提前到", "换成")),
        ("add_reminder", ("添加", "新增", "新建", "创建", "提醒我", "记一个", "加一个")),
    )
    for tool, keywords in keyword_groups:
        if any(keyword in normalized for keyword in keywords):
            return tool
    return ""


async def _repair_tool_routing(text: str, messages: list[dict], result: dict) -> tuple[dict, str]:
    """Force a second semantic pass when explicit mutation words and tool calls disagree."""
    expected = _mutation_tool_hint(text)
    if not expected or not llm_module:
        return result, expected

    calls = result.get("tool_calls", []) or []
    names = {call.get("function", {}).get("name", "") for call in calls}
    compatible = {expected}
    if expected in {"delete_reminder", "mark_done"}:
        compatible = {"delete_reminder", "mark_done"}
    if names & compatible:
        return result, expected

    from llm.openai_compatible import DDL_TOOLS
    selected = [tool for tool in DDL_TOOLS if tool["function"]["name"] == expected]
    forced_choice = {"type": "function", "function": {"name": expected}}
    route_instruction = (
        f"\n\n用户明确要求执行变更。必须调用 {expected}，"
        "根据用户原话和当前DDL数据提取准确参数；不要只用文字声称已完成。"
    )
    repair_messages = [dict(message) for message in messages]
    if repair_messages and repair_messages[0].get("role") == "system":
        repair_messages[0]["content"] = repair_messages[0].get("content", "") + route_instruction
    else:
        repair_messages.insert(0, {"role": "system", "content": route_instruction.strip()})
    logger.warning("Tool routing repair: transcript=%r initial=%s forced=%s",
                   text, sorted(names), expected)
    repaired = await llm_module.chat(repair_messages, selected, tool_choice=forced_choice)
    if repaired.get("tool_calls"):
        result = {**result, "tool_calls": repaired["tool_calls"]}
    elif expected in {"delete_reminder", "mark_done"} and store:
        # Some compatible providers ignore forced tool_choice and return prose.
        # Completion/removal still has a safe deterministic fallback because the
        # existing event itself supplies the only required argument.
        matches = await store.search(keyword=text)
        if matches:
            event = matches[0]
            canonical = " ".join(part for part in (event.course, event.title) if part)
            result = {**result, "tool_calls": [{
                "id": f"fallback_{event.id}",
                "type": "function",
                "function": {
                    "name": expected,
                    "arguments": json.dumps(
                        {"title_keyword": canonical}, ensure_ascii=False
                    ),
                },
            }]}
            logger.warning("Using deterministic %s fallback for DDL %s", expected, event.id)
    return result, expected


def _append_tool_results(messages: list[dict], calls: list[dict], reports: list[dict]) -> None:
    """Append one tool response per call as required by tool-calling APIs."""
    messages.append({"role": "assistant", "content": None, "tool_calls": calls})
    by_id = {report.get("tool_call_id", ""): report for report in reports}
    for call in calls:
        call_id = call.get("id", "")
        report = by_id.get(call_id, {})
        messages.append({
            "role": "tool",
            "content": report.get("message", "操作失败：服务器没有返回工具执行结果。"),
            "tool_call_id": call_id,
        })


# ── Reminder Handler ──────────────────────────────────────────

async def on_reminder_trigger(event: DDLItem):
    """Called by ReminderScheduler when a reminder is due."""
    logger.info(f"🔔 Reminder triggered: {event.title}")

    # Build reminder TTS text using human-readable duration
    dur = event.duration_str()
    if "OVERDUE" in dur:
        tts_text = f"提醒：{event.title}已经过期了！"
        emotion = "sad"
    elif "due now" in dur:
        tts_text = f"紧急提醒：{event.title}现在截止！"
        emotion = "surprised"
    else:
        tts_text = f"提醒：{event.title}还有{dur.replace(' left','')}截止"
        emotion = "surprised"

    # Synthesize reminder audio
    audio = await tts_module.synthesize(tts_text) if tts_module else b""

    event_dict = event.to_dict()

    # Phone-facing channels are independent of the ESP32 connection, so a
    # temporarily offline device does not lose the reminder.
    mobile_delivered = await send_mobile_notification(
        f"DDL提醒：{event.title}", f"{tts_text}\n截止时间：{event.deadline_str()}"
    )

    device_delivered = False
    for ws, state in connected_clients.items():
        try:
            # Send remind command
            await ws.send(json.dumps(msg_remind(event_dict), ensure_ascii=False))

            # Also send audio if available (PCM binary chunks + ping to end)
            if audio:
                await _send_speech(ws, state, tts_text, emotion, audio)

            # Flash LED red
            await ws.send(json.dumps(msg_led("flash", "#FF0000"), ensure_ascii=False))
            device_delivered = True
        except Exception as e:
            logger.error(f"Failed to notify client: {e}")
    return mobile_delivered or device_delivered


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
        await websocket.send(json.dumps(msg_asr_result("(未识别到语音)", True), ensure_ascii=False))
        await websocket.send(json.dumps(msg_speak("", "抱歉，我没有听清楚，请再说一遍。", "sad"), ensure_ascii=False))
        return

    # Send ASR result to ESP32 for on-screen display
    await websocket.send(json.dumps(msg_asr_result(transcript, True), ensure_ascii=False))

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

    # The first semantic pass may answer in prose despite an explicit mutation
    # verb. In that case, force a second LLM pass to extract arguments for the
    # appropriate tool; the keyword is only a guard, not the argument parser.
    result, expected_tool = await _repair_tool_routing(transcript, messages, result)

    # Step 3: Execute tool calls (with crash guard)
    tool_result = ""
    tool_reports = []
    original_tool_calls = result.get("tool_calls", [])  # Save BEFORE second LLM call
    if original_tool_calls and intent_parser:
        try:
            tool_result, tool_reports = await intent_parser.execute_detailed(original_tool_calls)
        except Exception as e:
            logger.error(f"Tool execution failed: {e}", exc_info=True)
            tool_result = f"操作失败: {e}"
            tool_reports = [{
                "tool": expected_tool or "unknown",
                "success": False,
                "message": tool_result,
                "tool_call_id": "",
            }]
        if tool_result:
            if any(report["tool"] in WRITE_TOOLS for report in tool_reports):
                # The spoken/UI confirmation must reflect the store result, not
                # a second model's potentially optimistic paraphrase.
                result["response"] = tool_result
                result["emotion"] = (
                    "happy" if all(report["success"] for report in tool_reports) else "sad"
                )
            else:
                _append_tool_results(messages, original_tool_calls, tool_reports)
                result = await llm_module.chat(messages, None)
    elif expected_tool:
        failure_message = "操作失败：大模型未能生成有效的工具参数，请换一种更明确的说法。"
        tool_reports = [{
            "tool": expected_tool,
            "success": False,
            "message": failure_message,
            "tool_call_id": "",
        }]
        result["response"] = failure_message
        result["emotion"] = "sad"

    # Update conversation history
    state.conversation_history.append({"role": "user", "content": transcript})
    state.conversation_history.append({"role": "assistant", "content": result["response"]})
    if len(state.conversation_history) > 20:
        state.conversation_history = state.conversation_history[-20:]

    # After DDL-modifying tools, push updated list to all clients
    if any(report["success"] and report["tool"] in WRITE_TOOLS for report in tool_reports):
        events = await store.get_pending()
        sync_data = [e.to_dict() for e in events]
        for ws_client in connected_clients:
            try:
                await ws_client.send(json.dumps(msg_sync(sync_data), ensure_ascii=False))
            except Exception:
                pass

    # Step 4: TTS — text to speech
    response_text = result["response"] or ""
    emotion = result.get("emotion", "neutral")

    await websocket.send(json.dumps(msg_emotion(emotion), ensure_ascii=False))

    audio = b""
    if response_text and tts_module and await tts_module.is_available():
        await websocket.send(json.dumps(msg_emotion("speaking"), ensure_ascii=False))
        audio = await tts_module.synthesize(response_text)

    # Step 5: Send response using explicit application-level stream boundaries.
    await _send_speech(websocket, state, response_text, emotion, audio)

    # Show feedback only for state-changing tools, and only after the answer has
    # been sent. Success means the EventStore operation actually returned true.
    for report in tool_reports:
        if report["tool"] in WRITE_TOOLS:
            await websocket.send(json.dumps(msg_tool_result(
                report["tool"], report["success"], report["message"]
            ), ensure_ascii=False))

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

        # Check for missed reminders (device was offline when reminder should have fired)
        missed = await store.get_missed_reminders()
        if missed:
            logger.info(f"🔔 Found {len(missed)} missed reminder(s) during offline period")
            for event in missed[:5]:  # cap at 5 to avoid flooding
                try:
                    await websocket.send(json.dumps(msg_remind(event.to_dict()), ensure_ascii=False))
                    dur = event.duration_str()
                    # TTS for missed reminder
                    if tts_module:
                        tts_text = f"提醒：你在离线时错过了 {event.title} 的提醒，{dur}"
                        audio = await tts_module.synthesize(tts_text)
                        await _send_speech(websocket, state, tts_text, "sad", audio)
                    else:
                        await _send_speech(
                            websocket, state,
                            f"提醒：你在离线时错过了 {event.title} 的提醒，{dur}",
                            "sad", b""
                        )
                    await store.mark_reminded(event.id)
                except Exception as e:
                    logger.error(f"Failed to send missed reminder: {e}")

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
                    result, expected_tool = await _repair_tool_routing(text, messages, result)

                    # Execute tool calls
                    original_tc = result.get("tool_calls", [])
                    tool_result = ""
                    tool_reports = []
                    if original_tc and intent_parser:
                        tool_result, tool_reports = await intent_parser.execute_detailed(original_tc)
                        if tool_result:
                            if any(report["tool"] in WRITE_TOOLS for report in tool_reports):
                                result["response"] = tool_result
                                result["emotion"] = (
                                    "happy" if all(report["success"] for report in tool_reports) else "sad"
                                )
                            else:
                                _append_tool_results(messages, original_tc, tool_reports)
                                result = await llm_module.chat(messages, None)
                    elif expected_tool:
                        failure_message = "操作失败：大模型未能生成有效的工具参数，请换一种更明确的说法。"
                        tool_reports = [{
                            "tool": expected_tool,
                            "success": False,
                            "message": failure_message,
                            "tool_call_id": "",
                        }]
                        result["response"] = failure_message
                        result["emotion"] = "sad"

                    # Sync after writes
                    if any(report["success"] and report["tool"] in WRITE_TOOLS for report in tool_reports):
                        events = await store.get_pending()
                        sync_data = [e.to_dict() for e in events]
                        for ws_client in connected_clients:
                            try:
                                await ws_client.send(json.dumps(msg_sync(sync_data), ensure_ascii=False))
                            except Exception:
                                pass

                    state.conversation_history.append({"role": "assistant", "content": result.get("response", "")})

                    response_text = result.get("response", "")
                    emotion = result.get("emotion", "neutral")

                    await websocket.send(json.dumps(msg_emotion(emotion), ensure_ascii=False))

                    # TTS
                    audio = b""
                    if response_text and tts_module:
                        audio = await tts_module.synthesize(response_text)

                    await _send_speech(websocket, state, response_text, emotion, audio)

                    for report in tool_reports:
                        if report["tool"] in WRITE_TOOLS:
                            await websocket.send(json.dumps(msg_tool_result(
                                report["tool"], report["success"], report["message"]
                            ), ensure_ascii=False))

                elif cmd == "event_action":
                    event_id, action = data.get("id", ""), data.get("action", "")
                    if action == "done":
                        await store.update_status(event_id, "done")
                        logger.info(f"✅ [{client_ip}] Marked done: {event_id}")
                    elif action == "snooze":
                        await store.update_status(event_id, "snoozed")
                        logger.info(f"⏰ [{client_ip}] Snoozed: {event_id}")
                    elif action.startswith("edit:"):
                        # Format: "edit:field_name=new_value"
                        parts = action[5:].split("=", 1)
                        if len(parts) == 2:
                            field, value = parts[0].strip(), parts[1].strip()
                            updates = {}
                            if field == "title":
                                updates["title"] = value
                            elif field == "course":
                                updates["course"] = value
                            elif field == "deadline":
                                try:
                                    from datetime import datetime, timezone, timedelta
                                    CST = timezone(timedelta(hours=8))
                                    dl = datetime.strptime(value, "%Y-%m-%dT%H:%M")
                                    updates["deadline"] = dl.replace(tzinfo=CST)
                                except ValueError:
                                    logger.warning(f"Invalid deadline format: {value}")
                            elif field == "advance":
                                try:
                                    minutes = max(0, int(value))
                                    updates["advance_minutes"] = minutes
                                    updates["reminder_minutes"] = [minutes]
                                    updates["remind_at_day_start"] = False
                                except ValueError:
                                    logger.warning(f"Invalid advance value: {value}")
                            elif field == "reminders":
                                try:
                                    # UI contract: at most six offsets, each within
                                    # 30 days + 23:59, with overlaps merged.
                                    values = sorted({
                                        min(44639, max(0, int(v)))
                                        for v in value.split(",") if v.strip()
                                    }, reverse=True)[:6]
                                    updates["reminder_minutes"] = values
                                    if values:
                                        updates["advance_minutes"] = values[0]
                                    updates["remind_at_day_start"] = False
                                except ValueError:
                                    logger.warning(f"Invalid reminder list: {value}")
                            if updates:
                                await store.update_event(event_id, updates)
                                logger.info(f"✏️ [{client_ip}] Edited {event_id}: {field}={value}")
                                # Sync back to all clients
                                events = await store.get_pending()
                                sync_data = [e.to_dict() for e in events]
                                for ws_client in connected_clients:
                                    try:
                                        await ws_client.send(json.dumps(
                                            msg_sync(sync_data), ensure_ascii=False))
                                    except Exception:
                                        pass

                elif cmd == "request_sync":
                    sync_started = time.monotonic()
                    events = await store.get_pending()
                    sync_data = [e.to_dict() for e in events]
                    await websocket.send(json.dumps(msg_sync(sync_data), ensure_ascii=False))
                    logger.info(
                        "🔄 [%s] DDL sync request served: %d events in %.1f ms",
                        client_ip, len(sync_data),
                        (time.monotonic() - sync_started) * 1000,
                    )

                elif cmd == "add_event":
                    # Direct DDL creation from ESP32 (touch form or voice)
                    title = data.get("title", "")
                    deadline_str = data.get("deadline", "")
                    course = data.get("course", "")
                    try:
                        from ddl.models import DDLItem
                        dl = datetime.fromisoformat(deadline_str)
                        item = DDLItem(
                            title=title or "未命名",
                            course=course or "",
                            type=data.get("type", "自定义"),
                            source="manual",
                            deadline=dl,
                            advance_minutes=data.get("advance_minutes", 1440),
                        )
                        await store.upsert(item)
                        logger.info(f"📝 [{client_ip}] Added DDL: {title}")

                        # Sync back to all clients
                        new_dict = item.to_dict()
                        for ws_client in connected_clients:
                            try:
                                await ws_client.send(json.dumps(
                                    msg_new_event(new_dict), ensure_ascii=False))
                            except Exception:
                                pass
                    except Exception as e:
                        logger.error(f"add_event failed: {e}")

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
    startup_removed = await store.cleanup_old(
        expired_days=EXPIRED_CLEANUP_DAYS,
        done_days=EXPIRED_CLEANUP_DAYS,
    )
    if startup_removed:
        logger.info("🧹 Startup cleanup removed %d records older than %d days",
                    startup_removed, EXPIRED_CLEANUP_DAYS)
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

    # Start periodic data cleanup. A cleanup also ran at startup, so restarting
    # the server does not postpone stale-record removal indefinitely.
    async def cleanup_loop():
        while True:
            await asyncio.sleep(CLEANUP_INTERVAL)
            try:
                removed = await store.cleanup_old(
                    expired_days=EXPIRED_CLEANUP_DAYS,
                    done_days=EXPIRED_CLEANUP_DAYS,
                )
                if removed > 0:
                    logger.info(f"🧹 Cleaned up {removed} old DDL records")
            except Exception as e:
                logger.error(f"Cleanup error: {e}")

    asyncio.create_task(cleanup_loop())
    logger.info(f"Data cleanup scheduled every {CLEANUP_INTERVAL//86400} days")

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
