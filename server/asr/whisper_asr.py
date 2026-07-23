"""
OpenAI Whisper ASR implementation.

Uses OpenAI Whisper API for speech recognition.
"""

import base64
import logging
import os

import httpx

from .base import BaseASR

logger = logging.getLogger(__name__)


class WhisperASR(BaseASR):
    """ASR via OpenAI Whisper API."""

    def __init__(self, api_key: str = ""):
        self.api_key = api_key or os.environ.get("OPENAI_API_KEY", "")
        self.api_url = "https://api.openai.com/v1/audio/transcriptions"

    async def is_available(self) -> bool:
        return bool(self.api_key)

    async def transcribe(self, audio_data: bytes, sample_rate: int = 16000) -> str:
        if not self.api_key:
            return ""

        headers = {"Authorization": f"Bearer {self.api_key}"}

        # Save PCM to temp WAV file
        import tempfile
        import wave

        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
            with wave.open(f, "wb") as wf:
                wf.setnchannels(1)
                wf.setsampwidth(2)
                wf.setframerate(sample_rate)
                wf.writeframes(audio_data)
            tmp_path = f.name

        try:
            async with httpx.AsyncClient(timeout=60) as client:
                with open(tmp_path, "rb") as f:
                    files = {
                        "file": ("audio.wav", f, "audio/wav"),
                        "model": (None, "whisper-1"),
                        "language": (None, "zh"),
                    }
                    resp = await client.post(self.api_url, headers=headers, files=files)
                    resp.raise_for_status()
                    result = resp.json()
                    text = result.get("text", "").strip()
                    logger.info(f"Whisper result: '{text}'")
                    return text
        except httpx.HTTPError as e:
            logger.error(f"Whisper ASR failed: {e}")
            return ""
        finally:
            try:
                os.unlink(tmp_path)
            except OSError:
                pass
