"""
JSON file-based event persistence.

Thread-safe with asyncio locks. Auto-saves on every mutation.
"""

import asyncio
import json
import os
from pathlib import Path

from .models import DDLItem


class EventStore:
    """Persistent event storage backed by a JSON file."""

    def __init__(self, filepath: str):
        self.filepath = Path(filepath)
        self._events: dict[str, DDLItem] = {}
        self._lock = asyncio.Lock()

    async def load(self) -> None:
        """Load events from JSON file. No-op if file doesn't exist."""
        async with self._lock:
            if not self.filepath.exists():
                return

            loop = asyncio.get_running_loop()
            data = await loop.run_in_executor(None, self._read_file)
            try:
                raw = json.loads(data)
            except json.JSONDecodeError:
                raw = []

            self._events.clear()
            for item_dict in raw:
                event = DDLItem.from_dict(item_dict)
                self._events[event.id] = event

    def _read_file(self) -> str:
        with open(self.filepath, "r", encoding="utf-8") as f:
            return f.read()

    async def _save(self) -> None:
        """Persist to disk. Called after every mutation."""
        data = [e.to_dict() for e in self._events.values()]
        loop = asyncio.get_running_loop()

        def _write():
            self.filepath.parent.mkdir(parents=True, exist_ok=True)
            with open(self.filepath, "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False, indent=2, default=str)

        await loop.run_in_executor(None, _write)

    # ── CRUD ──────────────────────────────────────────────────

    async def add(self, event: DDLItem) -> DDLItem:
        """Add a new event. Returns the event."""
        async with self._lock:
            self._events[event.id] = event
            await self._save()
        return event

    async def upsert(self, event: DDLItem) -> tuple[DDLItem, bool]:
        """
        Add or update an event. Deduplicates by (title, course, deadline).
        Returns (event, is_new).
        """
        async with self._lock:
            for existing in self._events.values():
                if (existing.title == event.title and
                        existing.course == event.course and
                        existing.deadline_iso() == event.deadline_iso()):
                    # Update existing, keep original id + status
                    event.id = existing.id
                    event.status = existing.status
                    event.reminder_sent = existing.reminder_sent
                    self._events[event.id] = event
                    await self._save()
                    return event, False

            # New event
            self._events[event.id] = event
            await self._save()
            return event, True

    async def remove(self, event_id: str) -> bool:
        """Remove an event by id. Returns True if removed."""
        async with self._lock:
            if event_id in self._events:
                del self._events[event_id]
                await self._save()
                return True
        return False

    async def update_status(self, event_id: str, status: str) -> bool:
        """Update event status (pending/done/snoozed/dismissed)."""
        async with self._lock:
            if event_id in self._events:
                self._events[event_id].status = status
                if status == "done":
                    self._events[event_id].reminder_sent = True  # don't remind again
                elif status == "snoozed":
                    self._events[event_id].reminder_sent = False  # remind again later
                await self._save()
                return True
        return False

    async def mark_reminded(self, event_id: str) -> bool:
        """Mark that a reminder has been sent."""
        async with self._lock:
            if event_id in self._events:
                self._events[event_id].reminder_sent = True
                await self._save()
                return True
        return False

    # ── Queries ───────────────────────────────────────────────

    async def get(self, event_id: str) -> DDLItem | None:
        async with self._lock:
            return self._events.get(event_id)

    async def get_all(self) -> list[DDLItem]:
        async with self._lock:
            events = list(self._events.values())
            events.sort(key=lambda e: e.deadline_iso())
            return events

    async def get_pending(self) -> list[DDLItem]:
        """Get all pending events (not done/dismissed)."""
        async with self._lock:
            return sorted(
                [e for e in self._events.values() if e.status in ("pending", "snoozed")],
                key=lambda e: e.deadline_iso()
            )

    async def search(self, keyword: str = "", days: int = 0) -> list[DDLItem]:
        """Search events by keyword. days=0 means all upcoming."""
        async with self._lock:
            results = [e for e in self._events.values() if e.status != "done"]
            if keyword:
                kw = keyword.lower()
                results = [e for e in results if kw in e.title.lower() or kw in e.course.lower()]
            if days > 0:
                from datetime import datetime, timezone, timedelta
                now = datetime.now(timezone.utc)
                cutoff = now + timedelta(days=days)
                results = [e for e in results if e.deadline.replace(tzinfo=timezone.utc) <= cutoff] if not isinstance(e.deadline, str) else [e for e in results]
            results.sort(key=lambda e: e.deadline_iso())
            return results

    async def count(self) -> int:
        async with self._lock:
            return len(self._events)
