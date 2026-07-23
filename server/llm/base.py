"""
Abstract LLM (Large Language Model) interface.

Supports function calling for structured DDL operations.
"""

from abc import ABC, abstractmethod
from typing import Any


class BaseLLM(ABC):
    """Abstract base class for LLM providers."""

    @abstractmethod
    async def chat(
        self,
        messages: list[dict],
        tools: list[dict] | None = None,
    ) -> dict:
        """
        Send conversation to LLM and get response.

        Args:
            messages: List of {"role": "...", "content": "..."} messages
            tools: Optional list of function definitions for function calling

        Returns:
            {
                "response": str,           # LLM's text response
                "tool_calls": list[dict],  # Function calls if any
                "emotion": str,            # Detected emotion tag
            }
        """
        ...

    @abstractmethod
    async def is_available(self) -> bool:
        """Check if the LLM service is ready."""
        ...
