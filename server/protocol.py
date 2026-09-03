"""
WebSocket message protocol definitions.

Format: All JSON messages have a "cmd" field.
Binary frames are audio data (PCM 16kHz, 16-bit, mono).
"""

# ── Server → ESP32 ────────────────────────────────────────────

def msg_sync(events: list[dict]) -> dict:
    """Full data sync — send all events to ESP32."""
    return {"cmd": "sync", "events": events}


def msg_new_event(event: dict) -> dict:
    """Notify ESP32 of a new DDL event."""
    return {"cmd": "new_event", "event": event}


def msg_delete_event(event_id: str) -> dict:
    """Tell ESP32 to remove an event."""
    return {"cmd": "delete_event", "id": event_id}


def msg_remind(event: dict) -> dict:
    """Trigger immediate reminder on ESP32."""
    return {"cmd": "remind", "event": event}


def msg_speak(audio_b64: str, text: str = "", emotion: str = "neutral") -> dict:
    """Send TTS audio + text + emotion to ESP32."""
    return {
        "cmd": "speak",
        "audio": audio_b64,
        "text": text,
        "emotion": emotion,
    }


def msg_emotion(emotion: str) -> dict:
    """Control avatar expression on ESP32."""
    return {"cmd": "emotion", "emotion": emotion}


def msg_led(action: str, color: str = "#FF0000") -> dict:
    """Control LED strip on ESP32."""
    return {"cmd": "led", "action": action, "color": color}


def msg_pong() -> dict:
    """Heartbeat response."""
    return {"cmd": "pong"}


def msg_config(key: str, value: str) -> dict:
    """Send config update to ESP32."""
    return {"cmd": "config", "key": key, "value": value}


# ── ESP32 → Server message parsers ────────────────────────────

def parse_message(raw: str) -> dict:
    """Parse a JSON text message from ESP32."""
    import json
    return json.loads(raw)


def is_audio_start(msg: dict) -> bool:
    return msg.get("cmd") == "audio_start"


def is_audio_end(msg: dict) -> bool:
    return msg.get("cmd") == "audio_end"


def is_query(msg: dict) -> bool:
    return msg.get("cmd") == "query"


def is_event_action(msg: dict) -> bool:
    return msg.get("cmd") == "event_action"


def is_request_sync(msg: dict) -> bool:
    return msg.get("cmd") == "request_sync"


def is_ping(msg: dict) -> bool:
    return msg.get("cmd") == "ping"


def get_query_text(msg: dict) -> str:
    return msg.get("text", "")


def get_event_action(msg: dict) -> tuple[str, str]:
    """Returns (event_id, action)."""
    return msg.get("id", ""), msg.get("action", "")


def msg_asr_result(text: str, is_final: bool = False) -> dict:
    """Send ASR transcription result to ESP32 for LCD display."""
    return {"cmd": "asr_result", "text": text, "final": is_final}


def msg_tool_result(tool: str, success: bool, message: str) -> dict:
    """Report a server-confirmed DDL tool execution to the device."""
    return {
        "cmd": "tool_result",
        "tool": tool,
        "success": bool(success),
        "message": message,
    }


def msg_audio_stream_start(stream_id: str, sample_rate: int = 16000) -> dict:
    """Begin one server-to-device PCM stream.

    This is deliberately an application message rather than a WebSocket ping:
    ping frames are also used automatically for connection keepalive.
    """
    return {
        "cmd": "audio_stream_start",
        "stream_id": stream_id,
        "format": "pcm16",
        "sample_rate": sample_rate,
    }


def msg_audio_stream_end(stream_id: str) -> dict:
    """End the matching server-to-device PCM stream."""
    return {"cmd": "audio_stream_end", "stream_id": stream_id}
