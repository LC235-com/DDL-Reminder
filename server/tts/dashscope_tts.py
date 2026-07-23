"""
DashScope CosyVoice TTS implementation.

Uses Alibaba Cloud DashScope CosyVoice API for high-quality TTS.
Requires DASHSCOPE_API_KEY.
"""

import base64
import io
import logging
import os
import wave

import httpx

from .base import BaseTTS

logger = logging.getLogger(__name__)

API_URL = "https://dashscope.aliyuncs.com/api/v1/services/audio/tts/synthesizer"


class DashScopeTTS(BaseTTS):
    """TTS via Alibaba DashScope CosyVoice API."""

    def __init__(self, api_key: str = "", voice: str = "longxiaochun"):
        self.api_key = api_key or os.environ.get("DASHSCOPE_API_KEY", "")
        self.voice = voice

    async def is_available(self) -> bool:
        return bool(self.api_key)

    async def synthesize(self, text: str) -> bytes:
        if not self.api_key:
            return b""

        headers = {
            "Authorization": f"Bearer {self.api_key}",
            "Content-Type": "application/json",
        }
        payload = {
            "model": "cosyvoice-v1",
            "input": {
                "text": text,
            },
            "parameters": {
                "voice": self.voice,
                "format": "wav",
                "sample_rate": 16000,
            },
        }

        async with httpx.AsyncClient(timeout=30) as client:
            try:
                resp = await client.post(API_URL, headers=headers, json=payload)
                resp.raise_for_status()
                result = resp.json()
                audio_b64 = result.get("output", {}).get("audio", "")
                if audio_b64:
                    # CosyVoice returns WAV or PCM bytes, decode
                    pcm_data = base64.b64decode(audio_b64)
                    logger.info(f"CosyVoice synthesized {len(pcm_data)} bytes")
                    # If it's WAV with header, strip to raw PCM
                    return self._extract_pcm(pcm_data)
                return b""
            except httpx.HTTPError as e:
                logger.error(f"DashScope TTS failed: {e}")
                return b""

    @staticmethod
    def _extract_pcm(data: bytes) -> bytes:
        """If WAV format, extract raw PCM; otherwise return as-is."""
        if data[:4] == b'RIFF':
            # It's a WAV file — read PCM data
            import io
            import wave
            with io.BytesIO(data) as f:
                with wave.open(f, "rb") as wf:
                    return wf.readframes(wf.getnframes())
        return data
