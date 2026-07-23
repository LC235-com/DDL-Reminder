"""
OpenAI-compatible LLM implementation.

Works with any provider that supports the OpenAI chat completions API:
- GLM-4-Flash (智谱)  → base_url: https://open.bigmodel.cn/api/paas/v4/
- DeepSeek            → base_url: https://api.deepseek.com/v1
- Qwen (阿里百炼)      → base_url: https://dashscope.aliyuncs.com/compatible-mode/v1
- OpenAI              → base_url: https://api.openai.com/v1

Configurable via LLM_CONFIGS in config.py.
"""

import logging
import os
import re

import httpx

from .base import BaseLLM

logger = logging.getLogger(__name__)

# Tool definitions for function calling
DDL_TOOLS = [
    {
        "type": "function",
        "function": {
            "name": "query_ddls",
            "description": "查询用户的DDL/待办事项。可以按关键词搜索或按天数筛选。",
            "parameters": {
                "type": "object",
                "properties": {
                    "keyword": {
                        "type": "string",
                        "description": "搜索关键词（可选），匹配标题和课程名",
                    },
                    "days": {
                        "type": "integer",
                        "description": "查询未来N天内的DDL，0=全部",
                    },
                },
                "required": [],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "add_reminder",
            "description": "为用户添加一条手动提醒事项。",
            "parameters": {
                "type": "object",
                "properties": {
                    "title": {"type": "string", "description": "提醒标题"},
                    "time": {
                        "type": "string",
                        "description": "截止时间，格式：YYYY-MM-DD HH:MM",
                    },
                    "course": {
                        "type": "string",
                        "description": "课程名称（可选）",
                    },
                },
                "required": ["title", "time"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "mark_done",
            "description": "标记某个DDL为已完成。",
            "parameters": {
                "type": "object",
                "properties": {
                    "title_keyword": {
                        "type": "string",
                        "description": "要标记完成的DDL标题关键词",
                    },
                },
                "required": ["title_keyword"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "get_courses",
            "description": "列出所有有DDL的课程名称。",
            "parameters": {
                "type": "object",
                "properties": {},
                "required": [],
            },
        },
    },
]

# Emotion tags the LLM can output
EMOTION_TAGS = ["happy", "neutral", "thinking", "surprised", "sad"]


class OpenAICompatibleLLM(BaseLLM):
    """LLM via any OpenAI-compatible chat completions API."""

    def __init__(self, config: dict):
        """
        Args:
            config: {
                "base_url": "https://...",
                "model": "glm-4-flash",
                "api_key": "sk-...",
            }
        """
        self.base_url = config.get("base_url", "").rstrip("/")
        self.model = config.get("model", "glm-4-flash")
        self.api_key = config.get("api_key", "")
        self.temperature = config.get("temperature", 0.7)
        self.max_tokens = config.get("max_tokens", 1024)

    async def is_available(self) -> bool:
        return bool(self.api_key) and bool(self.base_url)

    async def chat(
        self,
        messages: list[dict],
        tools: list[dict] | None = None,
    ) -> dict:
        """
        Send chat request. Returns {"response": str, "tool_calls": list, "emotion": str}.
        """
        if not self.api_key:
            return {"response": "LLM API key not configured.", "tool_calls": [], "emotion": "sad"}

        url = f"{self.base_url}/chat/completions"
        headers = {
            "Authorization": f"Bearer {self.api_key}",
            "Content-Type": "application/json",
        }

        payload: dict[str, Any] = {
            "model": self.model,
            "messages": messages,
            "temperature": self.temperature,
            "max_tokens": self.max_tokens,
        }

        # Add tools if supported
        if tools is not None:
            payload["tools"] = tools
            payload["tool_choice"] = "auto"

        async with httpx.AsyncClient(timeout=60) as client:
            try:
                resp = await client.post(url, headers=headers, json=payload)
                resp.raise_for_status()
                result = resp.json()
            except httpx.HTTPError as e:
                logger.error(f"LLM API error: {e}")
                return {"response": f"抱歉，AI服务暂时不可用。{e}", "tool_calls": [], "emotion": "sad"}

        choice = result.get("choices", [{}])[0]
        msg = choice.get("message", {})

        response_text = msg.get("content", "") or ""
        tool_calls = msg.get("tool_calls", []) or []

        # Parse emotion from response text
        emotion = self._parse_emotion(response_text)

        # Strip emotion tags from response
        clean_text = self._strip_emotion_tags(response_text)

        logger.info(f"LLM response (emotion={emotion}): '{clean_text[:100]}...'")

        return {
            "response": clean_text,
            "tool_calls": tool_calls,
            "emotion": emotion,
        }

    @staticmethod
    def _parse_emotion(text: str) -> str:
        """Extract [emotion:xxx] tag from text."""
        match = re.search(r'\[emotion:(\w+)\]', text)
        if match and match.group(1) in EMOTION_TAGS:
            return match.group(1)
        return "neutral"

    @staticmethod
    def _strip_emotion_tags(text: str) -> str:
        """Remove [emotion:xxx] tags from text."""
        return re.sub(r'\[emotion:\w+\]\s*', '', text).strip()
