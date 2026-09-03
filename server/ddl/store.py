"""
JSON file-based event persistence.

Thread-safe with asyncio locks. Auto-saves on every mutation.
"""

import asyncio
import json
import os
import re
import unicodedata
from difflib import SequenceMatcher
from pathlib import Path
from datetime import datetime, timezone, timedelta

from .models import DDLItem


def _normalize_match_text(value: str) -> str:
    """Normalize ASR/tool text without losing Chinese letters or digits."""
    value = unicodedata.normalize("NFKC", value or "").casefold()
    value = re.sub(r"[^\w]+", "", value, flags=re.UNICODE)
    # Spoken Chinese frequently inserts this connector between course/title.
    return value.replace("_", "").replace("的", "")


def _match_score(event: DDLItem, keyword: str) -> int:
    """Rank a keyword against title, course and their combined display name."""
    query = _normalize_match_text(keyword)
    if not query:
        return 1

    title = _normalize_match_text(event.title)
    course = _normalize_match_text(event.course)
    combined = course + title
    fields = [value for value in (title, course, combined) if value]

    if query == combined or query == title:
        return 1000
    if course and title and course in query and title in query:
        return 980
    if query in title:
        return 940
    if query in combined:
        return 920
    if title and title in query:
        return 880

    # Handle small ASR errors and utterances which omit a suffix such as “时间”.
    best_ratio = max((SequenceMatcher(None, query, value).ratio() for value in fields), default=0.0)
    title_match = SequenceMatcher(None, query, title).find_longest_match() if title else None
    common_title_chars = title_match.size if title_match else 0
    if course and course in query and common_title_chars >= min(4, len(title)):
        return 840 + min(common_title_chars, 20)
    if best_ratio >= 0.72:
        return int(700 * best_ratio)
    return 0


def _deadline_utc(event: DDLItem) -> datetime:
    value = datetime.fromisoformat(event.deadline) if isinstance(event.deadline, str) else event.deadline
    if value.tzinfo is None:
        value = value.replace(tzinfo=timezone.utc)
    return value.astimezone(timezone.utc)


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
                    event.sent_reminders = existing.sent_reminders
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

    async def mark_reminded(self, event_id: str, reminder_keys: list[str] | None = None) -> bool:
        """Mark one or more schedule occurrences as sent."""
        async with self._lock:
            if event_id in self._events:
                event = self._events[event_id]
                if reminder_keys is None:
                    # Compatibility for older callers: mark all occurrences due now.
                    from datetime import datetime, timezone
                    now = datetime.now(timezone.utc)
                    reminder_keys = [key for key, when in event.reminder_schedule() if when <= now]
                event.sent_reminders = list(dict.fromkeys(event.sent_reminders + reminder_keys))
                event.reminder_sent = all(
                    key in event.sent_reminders for key, _ in event.reminder_schedule()
                )
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
            if any(key in updates for key in ("deadline", "advance_minutes", "reminder_minutes", "remind_at_day_start")):
                e.sent_reminders = []
                e.reminder_sent = False
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
        """Get every unfinished, not-yet-expired event in deadline order."""
        async with self._lock:
            now = datetime.now(timezone.utc)
            def is_upcoming(event: DDLItem) -> bool:
                return _deadline_utc(event) >= now
            return sorted(
                [e for e in self._events.values()
                 if e.status in ("pending", "snoozed") and is_upcoming(e)],
                key=lambda e: e.deadline_iso()
            )

    async def search(self, keyword: str = "", days: int = 0) -> list[DDLItem]:
        """Search upcoming events across title+course, tolerant of ASR spacing/errors."""
        async with self._lock:
            now = datetime.now(timezone.utc)
            results = []
            for event in self._events.values():
                if event.status not in ("pending", "snoozed"):
                    continue
                if _deadline_utc(event) >= now:
                    results.append(event)
            if keyword:
                scored = [(_match_score(event, keyword), event) for event in results]
                results = [event for score, event in scored if score > 0]
                scores = {event.id: score for score, event in scored if score > 0}
            if days > 0:
                cutoff = now + timedelta(days=days)
                results = [e for e in results if _deadline_utc(e) <= cutoff]
            if keyword:
                results.sort(key=lambda e: (-scores[e.id], e.deadline_iso()))
            else:
                results.sort(key=lambda e: e.deadline_iso())
            return results

    async def count(self) -> int:
        async with self._lock:
            return len(self._events)

    async def cleanup_old(self, expired_days: int = 14, done_days: int = 14) -> int:
        """
        Remove old DDLs:
        - Events marked 'done' older than done_days
        - Events whose deadline passed more than expired_days ago
        - Events marked 'deleted'

        Returns number of events removed.
        """
        from datetime import datetime, timezone
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
                if isinstance(e.deadline, str):
                    try:
                        dl = datetime.fromisoformat(e.deadline)
                    except (ValueError, TypeError):
                        continue
                else:
                    dl = e.deadline
                if dl.tzinfo is None:
                    dl = dl.replace(tzinfo=timezone.utc)

                has_due = any(
                    when <= now and key not in e.sent_reminders
                    for key, when in e.reminder_schedule()
                )
                if has_due and now < dl:
                    missed.append(e)

            # Sort by most urgent first
            missed.sort(key=lambda e: e.deadline_iso())
        return missed
