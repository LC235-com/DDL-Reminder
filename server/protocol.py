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
