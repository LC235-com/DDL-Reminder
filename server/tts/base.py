"""
Abstract TTS (Text-to-Speech) interface.
"""

from abc import ABC, abstractmethod


class BaseTTS(ABC):
    """Abstract base class for TTS providers."""

    @abstractmethod
    async def synthesize(self, text: str) -> bytes:
        """
        Convert text to PCM16 audio (16kHz, mono).

        Args:
            text: Text to synthesize

        Returns:
            Raw PCM16 audio bytes (16kHz, mono, 16-bit little-endian)
        """
        ...

    @abstractmethod
    async def is_available(self) -> bool:
        """Check if the TTS service is ready."""
        ...
