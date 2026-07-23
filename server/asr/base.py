"""
Abstract ASR (Automatic Speech Recognition) interface.
"""

from abc import ABC, abstractmethod


class BaseASR(ABC):
    """Abstract base class for ASR providers."""

    @abstractmethod
    async def transcribe(self, audio_data: bytes, sample_rate: int = 16000) -> str:
        """
        Convert PCM audio to text.

        Args:
            audio_data: Raw PCM audio bytes (16-bit, mono)
            sample_rate: Audio sample rate in Hz

        Returns:
            Transcribed text string (empty if nothing recognized)
        """
        ...

    @abstractmethod
    async def is_available(self) -> bool:
        """Check if the ASR service is ready to use."""
        ...
