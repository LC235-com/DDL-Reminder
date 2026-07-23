"""
FunASR SenseVoiceSmall ASR implementation (recommended default).

Local, free, no API key needed. Supports 5 languages.
Model auto-downloads on first use.

Reference: 小智AI uses this exact setup.
"""

import logging
import os

from .base import BaseASR

logger = logging.getLogger(__name__)


class FunASR(BaseASR):
    """Local ASR using FunASR SenseVoiceSmall model."""

    # FunASR 1.x uses ModelScope model IDs for auto-download
    MODEL_NAME = "iic/SenseVoiceSmall"

    def __init__(self, model_name: str = ""):
        self.model_name = model_name or self.MODEL_NAME
        self._model = None
        self._available = False
        self._init_model()

    def _init_model(self):
        """Initialize the FunASR model (auto-downloads from ModelScope on first use)."""
        try:
            from funasr import AutoModel
        except ModuleNotFoundError as e:
            missing = e.name if hasattr(e, 'name') and e.name else str(e)
            if missing == "funasr":
                logger.warning("funasr not installed. Install with: pip install funasr")
            elif missing == "torch":
                logger.warning("PyTorch not found. Install with: pip install torch torchaudio")
            elif missing == "torchaudio":
                logger.warning("torchaudio not found. Install with: pip install torchaudio")
            else:
                logger.warning(f"FunASR import failed: missing module '{missing}'. "
                               f"Install with: pip install {missing}")
            logger.warning("Falling back to other ASR providers.")
            return
        except ImportError as e:
            logger.warning(f"FunASR import failed: {e}")
            logger.warning("Falling back to other ASR providers.")
            return

        try:
            logger.info(f"Loading FunASR model '{self.model_name}' (first run will download ~200MB)...")
            self._model = AutoModel(
                model=self.model_name,
                device="cpu",
                disable_update=True,
            )
            self._available = True
            logger.info("FunASR SenseVoiceSmall loaded successfully")
        except Exception as e:
            logger.warning(f"FunASR model init failed: {e}")
            logger.warning(
                "Make sure the path is writable and network is available. "
                "Model will auto-download from ModelScope on first use."
            )

    async def is_available(self) -> bool:
        return self._available

    async def transcribe(self, audio_data: bytes, sample_rate: int = 16000) -> str:
        if not self._model:
            logger.warning("FunASR model not loaded — cannot transcribe")
            return ""

        import tempfile
        import wave
        import asyncio

        try:
            # Write PCM to temp WAV file
            with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
                with wave.open(f, "wb") as wf:
                    wf.setnchannels(1)
                    wf.setsampwidth(2)
                    wf.setframerate(sample_rate)
                    wf.writeframes(audio_data)
                tmp_path = f.name

            # Run ASR in thread pool (FunASR generate() is blocking)
            loop = asyncio.get_running_loop()
            result = await loop.run_in_executor(
                None,
                lambda: self._model.generate(input=tmp_path, language="zh")
            )

            # Cleanup temp file
            try:
                os.unlink(tmp_path)
            except OSError:
                pass

            if result and len(result) > 0:
                text = result[0].get("text", "")
                logger.info(f"FunASR result: '{text}'")
                return text
            return ""

        except Exception as e:
            logger.error(f"FunASR transcription failed: {e}")
            return ""
