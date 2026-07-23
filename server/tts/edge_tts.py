"""
Edge-TTS implementation (free, Microsoft).

Uses Microsoft Edge TTS service. No API key required.
Default voice: zh-CN-XiaoxiaoNeural (natural female voice).

Reference: 小智AI uses this as default TTS.
"""

import asyncio
import io
import logging
import tempfile
import os
import wave

from .base import BaseTTS

logger = logging.getLogger(__name__)


class EdgeTTS(BaseTTS):
    """Free TTS via Microsoft Edge TTS service."""

    def __init__(self, voice: str = "zh-CN-XiaoxiaoNeural"):
        self.voice = voice
        self._available = False
        self._check_available()

    def _check_available(self):
        try:
            import edge_tts
            self._available = True
            logger.info(f"Edge-TTS ready (voice: {self.voice})")
        except ImportError:
            logger.warning("edge-tts not installed. Install: pip install edge-tts")

    async def is_available(self) -> bool:
        return self._available

    async def synthesize(self, text: str) -> bytes:
        """Synthesize text to PCM audio bytes using Edge-TTS."""
        if not self._available:
            return b""

        try:
            import edge_tts

            # Generate MP3 to temp file
            with tempfile.NamedTemporaryFile(suffix=".mp3", delete=False) as f:
                tmp_mp3 = f.name

            communicate = edge_tts.Communicate(text, self.voice)
            await communicate.save(tmp_mp3)

            # Convert MP3 to PCM 16kHz mono
            pcm = await asyncio.to_thread(self._mp3_to_pcm, tmp_mp3)

            # Cleanup
            try:
                os.unlink(tmp_mp3)
            except OSError:
                pass

            return pcm

        except Exception as e:
            logger.error(f"Edge-TTS failed: {e}")
            return b""

    @staticmethod
    def _mp3_to_pcm(mp3_path: str, target_rate: int = 16000) -> bytes:
        """Convert MP3 file to PCM16 16kHz mono."""
        try:
            from pydub import AudioSegment

            audio = AudioSegment.from_mp3(mp3_path)
            audio = audio.set_frame_rate(target_rate)
            audio = audio.set_channels(1)
            audio = audio.set_sample_width(2)  # 16-bit
            return audio.raw_data
        except ImportError:
            logger.error("pydub not installed. Install: pip install pydub")
            # Fallback: just read raw bytes
            with open(mp3_path, "rb") as f:
                return f.read()
