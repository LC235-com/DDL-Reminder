"""
DashScope Paraformer ASR implementation.

Uses Alibaba Cloud DashScope API for speech recognition.
Requires DASHSCOPE_API_KEY environment variable.

Two modes:
  1. DashScope Python SDK (recommended: pip install dashscope)
  2. Direct REST API fallback
"""

import base64
import logging
import os
import time

import httpx

from .base import BaseASR

logger = logging.getLogger(__name__)

# Correct DashScope REST API endpoint for speech recognition
# Note: "recognition" not "recognize" (the old /recognize endpoint is deprecated)
API_URL = "https://dashscope.aliyuncs.com/api/v1/services/audio/asr/recognition"


class DashScopeASR(BaseASR):
    """ASR via Alibaba DashScope Paraformer API."""

    def __init__(self, api_key: str = ""):
        self.api_key = api_key or os.environ.get("DASHSCOPE_API_KEY", "")
        self._use_sdk = False
        self._check_sdk()

    def _check_sdk(self):
        """Check if dashscope SDK is available."""
        try:
            import dashscope
            dashscope.base.http_client  # verify import works
            self._use_sdk = True
            logger.info("DashScope SDK available, using SDK mode")
        except ImportError:
            logger.info("DashScope SDK not installed, using REST API mode "
                        "(install with: pip install dashscope)")
        except Exception:
            logger.info("DashScope SDK init failed, using REST API mode")

    async def is_available(self) -> bool:
        return bool(self.api_key)

    async def transcribe(self, audio_data: bytes, sample_rate: int = 16000) -> str:
        if not self.api_key:
            logger.warning("DashScope API key not set — cannot transcribe")
            return ""

        if self._use_sdk:
            return await self._transcribe_sdk(audio_data, sample_rate)
        else:
            return await self._transcribe_rest(audio_data, sample_rate)

    async def _transcribe_sdk(self, audio_data: bytes, sample_rate: int) -> str:
        """Use DashScope Python SDK for recognition."""
        import asyncio
        import dashscope
        from dashscope.audio.asr import Recognition

        dashscope.api_key = self.api_key

        # Recognition.call() is synchronous, run in executor
        loop = asyncio.get_running_loop()
        try:
            result = await loop.run_in_executor(
                None,
                lambda: Recognition.call(
                    model="paraformer-v1",
                    format="pcm",
                    sample_rate=sample_rate,
                    audio=base64.b64encode(audio_data).decode("utf-8"),
                ),
            )

            if result.status_code == 200:
                text = result.get_output().get("text", "")
                logger.info(f"ASR result: '{text}'")
                return text
            else:
                logger.error(f"ASR SDK error: code={result.status_code}, "
                             f"message={result.message}")
                return ""
        except Exception as e:
            logger.error(f"DashScope SDK ASR failed: {e}")
            return ""

    async def _transcribe_rest(self, audio_data: bytes, sample_rate: int) -> str:
        """Use DashScope REST API directly (fallback when SDK not installed)."""
        audio_b64 = base64.b64encode(audio_data).decode("utf-8")

        headers = {
            "Authorization": f"Bearer {self.api_key}",
            "Content-Type": "application/json",
        }
        payload = {
            "model": "paraformer-v1",
            "input": {
                "audio": audio_b64,
            },
            "parameters": {
                "format": "pcm",
                "sample_rate": sample_rate,
                "language_hints": ["zh", "en"],
            },
        }

        async with httpx.AsyncClient(timeout=30) as client:
            try:
                resp = await client.post(API_URL, headers=headers, json=payload)
                if resp.status_code != 200:
                    logger.error(f"ASR HTTP {resp.status_code}: {resp.text[:300]}")
                    return ""
                result = resp.json()
                # DashScope returns: {"output": {"text": "..."}} or
                # {"output": {"sentence": {"text": "..."}}}
                output = result.get("output", {})
                text = output.get("text", "") or output.get("sentence", {}).get("text", "")
                logger.info(f"ASR result: '{text}'")
                return text
            except Exception as e:
                logger.error(f"DashScope ASR failed: {e}")
                return ""
