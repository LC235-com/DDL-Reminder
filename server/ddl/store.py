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

    async def update_event(self, event_id: str, updates: dict) -> bool:
        """Update individual fields of an event by id."""
        async with self._lock:
            if event_id not in self._events:
                return False
            e = self._events[event_id]
            for key, value in updates.items():
                if hasattr(e, key):
                    setattr(e, key, value)
            await self._save()
            return True

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
                results = [e for e in results if (e.title and kw in e.title.lower()) or (e.course and kw in e.course.lower())]
            if days > 0:
                from datetime import datetime, timezone, timedelta
                now = datetime.now(timezone.utc)
                cutoff = now + timedelta(days=days)
                results = [e for e in results if isinstance(e.deadline, str) or e.deadline.replace(tzinfo=timezone.utc) <= cutoff]
            results.sort(key=lambda e: e.deadline_iso())
            return results

    async def count(self) -> int:
        async with self._lock:
            return len(self._events)

    async def cleanup_old(self, expired_days: int = 3, done_days: int = 3) -> int:
        """
        Remove old DDLs:
        - Events marked 'done' older than done_days
        - Events whose deadline passed more than expired_days ago
        - Events marked 'deleted'

        Returns number of events removed.
        """
        from datetime import datetime, timezone, timedelta
        now = datetime.now(timezone.utc)
        expired_cutoff = now - timedelta(days=expired_days)
        done_cutoff = now - timedelta(days=done_days)

        removed = []
        async with self._lock:
            for eid, e in list(self._events.items()):
                # Remove explicitly deleted
                if e.status == "deleted":
                    removed.append(eid)
                    continue

                # Parse deadline
                if isinstance(e.deadline, str):
                    try:
                        dl = datetime.fromisoformat(e.deadline)
                    except (ValueError, TypeError):
                        continue
                else:
                    dl = e.deadline
                if dl.tzinfo is None:
                    dl = dl.replace(tzinfo=timezone.utc)

                # Remove done events older than cutoff
                if e.status == "done":
                    # Use deadline as reference for cleanup
                    if dl < done_cutoff:
                        removed.append(eid)
                    continue

                # Remove expired events (overdue by more than expired_days)
                if dl < expired_cutoff and e.status in ("pending", "snoozed"):
                    removed.append(eid)

            for eid in removed:
                del self._events[eid]

            if removed:
                await self._save()
        return len(removed)

    async def get_missed_reminders(self) -> list:
        """
        Find DDLs whose reminder time passed while device was offline.
        Returns events that:
        - Are still pending/snoozed
        - Have reminder_sent = False
        - Have deadline - advance_minutes < now (reminder time passed)
        """
        from datetime import datetime, timezone, timedelta
        now = datetime.now(timezone.utc)
        missed = []

        async with self._lock:
            for e in self._events.values():
                if e.status not in ("pending", "snoozed"):
                    continue
                if e.reminder_sent:
                    continue

                if isinstance(e.deadline, str):
                    try:
                        dl = datetime.fromisoformat(e.deadline)
                    except (ValueError, TypeError):
                        continue
                else:
                    dl = e.deadline
                if dl.tzinfo is None:
                    dl = dl.replace(tzinfo=timezone.utc)

                reminder_time = dl - timedelta(minutes=e.advance_minutes)
                if now >= reminder_time and now < dl:
                    missed.append(e)

            # Sort by most urgent first
            missed.sort(key=lambda e: e.deadline_iso())
        return missed
