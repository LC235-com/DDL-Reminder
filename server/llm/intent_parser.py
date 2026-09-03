"""
Intent parser — parses LLM function call responses into structured actions.

Handles:
- query_ddls → search the event store
- add_reminder → add a new event
- mark_done → mark event complete
- get_courses → list unique courses
"""

import json
import logging
import os
import sys
from datetime import datetime, timezone, timedelta

# Ensure server root is in path for absolute imports
_server_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _server_dir not in sys.path:
    sys.path.insert(0, _server_dir)

from ddl.models import DDLItem

logger = logging.getLogger(__name__)

CST = timezone(timedelta(hours=8))


def _parse_deadline(value: str) -> datetime:
    """Accept both tool-friendly `YYYY-MM-DD HH:MM` and ISO-8601 input."""
    normalized = value.strip().replace("Z", "+00:00")
    try:
        deadline = datetime.fromisoformat(normalized)
    except ValueError:
        deadline = datetime.strptime(normalized, "%Y-%m-%d %H:%M")
    if deadline.tzinfo is None:
        return deadline.replace(tzinfo=CST)
    return deadline.astimezone(CST)


class IntentParser:
    """Parse LLM tool calls and execute them against the event store."""

    def __init__(self, store):
        self.store = store

    async def execute(self, tool_calls: list[dict]) -> str:
        """
        Execute tool calls and return a text summary of results.

        Args:
            tool_calls: List of tool call dicts from LLM response

        Returns:
            Text summary to feed back to LLM as function result
        """
        text, _ = await self.execute_detailed(tool_calls)
        return text

    async def execute_detailed(self, tool_calls: list[dict]) -> tuple[str, list[dict]]:
        """Execute calls and return both LLM text and verifiable per-call reports."""
        results = []
        reports = []

        for call in tool_calls:
            func = call.get("function", {})
            name = func.get("name", "")
            args_str = func.get("arguments", "{}")

            try:
                args = json.loads(args_str)
            except json.JSONDecodeError:
                args = {}

            try:
                if name == "query_ddls":
                    text = await self._query_ddls(args)
                elif name == "add_reminder":
                    text = await self._add_reminder(args)
                elif name == "mark_done":
                    text = await self._mark_done(args)
                elif name == "modify_reminder":
                    text = await self._modify_reminder(args)
                elif name == "delete_reminder":
                    text = await self._mark_done(args)
                elif name == "get_courses":
                    text = await self._get_courses()
                else:
                    text = f"操作失败：未知工具 {name or '(空)'}。"
                    logger.warning("Unknown tool call: %s", name)
            except Exception as exc:
                logger.exception("Tool %s execution failed", name)
                text = f"操作失败：{exc}"

            success = not text.startswith(("操作失败", "添加提醒需要", "请指定", "无法解析", "没有找到"))
            results.append(text)
            report = {
                "tool_call_id": call.get("id", ""),
                "tool": name,
                "success": success,
                "message": text,
            }
            reports.append(report)
            logger.info("Tool result: %s success=%s args=%s result=%s",
                        name, success, args, text)

        return "\n".join(results), reports

    async def _query_ddls(self, args: dict) -> str:
        keyword = args.get("keyword", "")
        days = args.get("days", 0)
        events = await self.store.search(keyword=keyword, days=days)

        if not events:
            return "当前没有找到匹配的DDL。"

        lines = [f"找到 {len(events)} 条DDL："]
        for e in events[:10]:
            status = e.duration_str()
            lines.append(
                f"- [{e.tag()}] {e.course} - {e.title} ({e.deadline_str()}, {status})"
            )
        if len(events) > 10:
            lines.append(f"...还有 {len(events) - 10} 条")
        return "\n".join(lines)

    async def _add_reminder(self, args: dict) -> str:
        title = args.get("title", "")
        time_str = args.get("time", "")
        course = args.get("course", "")

        if not title or not time_str:
            return "添加提醒需要标题和时间。"

        try:
            deadline = _parse_deadline(time_str)
        except ValueError:
            return f"无法解析时间 '{time_str}'，请使用 YYYY-MM-DD HH:MM 格式。"

        event = DDLItem(
            title=title,
            course=course,
            type="自定义",
            source="manual",
            deadline=deadline,
            advance_minutes=1440,
        )
        existing = await self.store.search(keyword=title)
        exact = next((item for item in existing if item.title.strip().lower() == title.strip().lower()), None)
        if exact:
            updated = await self.store.update_event(
                exact.id, {"deadline": deadline, "course": course or exact.course}
            )
            if not updated:
                return f"操作失败：无法更新已有DDL“{title}”。"
            return f"✅ 已存在同名DDL，已更新截止时间：{title}，{time_str}"
        saved = await self.store.add(event)
        if not saved:
            return f"操作失败：无法添加DDL“{title}”。"
        return f"✅ 已添加提醒：{title}，截止时间 {time_str}"

    async def _mark_done(self, args: dict) -> str:
        keyword = args.get("title_keyword", "")
        if not keyword:
            return "请指定要完成的DDL标题。"

        events = await self.store.search(keyword=keyword)
        if not events:
            return f"没有找到包含 '{keyword}' 的DDL。"

        event = events[0]
        if not await self.store.update_status(event.id, "done"):
            return f"操作失败：无法更新DDL“{event.title}”。"
        return f"✅ 已标记完成：{event.title}"

    async def _modify_reminder(self, args: dict) -> str:
        keyword = (args.get("title_keyword") or "").strip()
        new_title = (args.get("new_title") or "").strip()
        new_time = (args.get("new_time") or "").strip()
        new_course = (args.get("new_course") or "").strip()

        if not keyword:
            return "请指定要修改的DDL标题关键词。"

        events = await self.store.search(keyword=keyword)
        if not events:
            # Voice users often say "把 X 改到明天" even when X has not been
            # synced yet. With a concrete new time this should create X instead
            # of silently doing nothing.
            if not new_time:
                return f"没有找到包含 '{keyword}' 的DDL，而且没有提供可用于新建的截止时间。"
            try:
                deadline = _parse_deadline(new_time)
            except ValueError:
                return f"无法解析时间 '{new_time}'，请使用 YYYY-MM-DD HH:MM 格式。"
            title = new_title or keyword
            event = DDLItem(
                title=title,
                course=new_course,
                type="自定义",
                source="voice",
                deadline=deadline,
            )
            saved = await self.store.add(event)
            if not saved:
                return f"操作失败：无法新建DDL“{title}”。"
            return f"✅ 未找到原DDL，已新建：{title}，截止时间 {new_time}"

        # Update the first matching event
        event = events[0]
        updates = {}
        if new_title:
            updates["title"] = new_title
        if new_time:
            try:
                updates["deadline"] = _parse_deadline(new_time)
            except ValueError:
                return f"无法解析时间 '{new_time}'，请使用 YYYY-MM-DD HH:MM 格式。"
        if new_course:
            updates["course"] = new_course

        if not updates:
            return f"请指定要修改的内容（新标题、新时间或新课程）。"

        if not await self.store.update_event(event.id, updates):
            return f"操作失败：无法修改DDL“{event.title}”。"
        return f"✅ 已修改DDL：{event.title}" + (
            f" → {new_title}" if new_title else ""
        ) + (
            f"，截止时间改为 {new_time}" if new_time else ""
        )

    async def _get_courses(self) -> str:
        events = await self.store.get_all()
        courses = list(set(e.course for e in events if e.course))
        if not courses:
            return "当前没有课程记录。"
        return "当前有DDL的课程：" + "、".join(courses[:10])
