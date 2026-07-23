"""
Reminder scheduler — periodically checks for events that need reminding.

Runs as a background asyncio task. When an event's reminder time arrives,
notifies all connected WebSocket clients.
"""

import asyncio
import logging
from datetime import datetime, timezone, timedelta

from .models import DDLItem
from .store import EventStore

logger = logging.getLogger(__name__)


class ReminderScheduler:
    """
    Background task that checks for due reminders.

    For each pending event, checks if:
        now >= deadline - advance_minutes

    When triggered, calls the on_remind callback with the event.
    """

    def __init__(self, store: EventStore, interval: int = 30):
        """
        Args:
            store: EventStore instance
            interval: Check interval in seconds (default 30)
        """
        self.store = store
        self.interval = interval
        self._task: asyncio.Task | None = None
        self._callbacks: list = []  # list of async callable(event)
        self._running = False

    def on_remind(self, callback):
        """Register a callback: async def callback(event: DDLItem, clients: list)."""
        self._callbacks.append(callback)

    async def start(self):
        """Start the reminder loop."""
        self._running = True
        self._task = asyncio.create_task(self._loop())
        logger.info(f"Reminder scheduler started (interval={self.interval}s)")

    async def stop(self):
        """Stop the reminder loop."""
        self._running = False
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
        logger.info("Reminder scheduler stopped")

    async def check_now(self) -> list[DDLItem]:
        """
        Check for due reminders right now (one-shot).
        Returns list of events that need reminding.
        """
        due = []
        pending = await self.store.get_pending()
        now = datetime.now(timezone.utc)

        for event in pending:
            if event.reminder_sent:
                continue
            if isinstance(event.deadline, str):
                deadline = datetime.fromisoformat(event.deadline)
            else:
                deadline = event.deadline
            if deadline.tzinfo is None:
                deadline = deadline.replace(tzinfo=timezone.utc)

            reminder_time = deadline - timedelta(minutes=event.advance_minutes)
            if now >= reminder_time:
                due.append(event)

        return due

    async def _loop(self):
        """Main loop — check every `interval` seconds."""
        while self._running:
            try:
                due = await self.check_now()
                if due:
                    logger.info(f"Found {len(due)} event(s) needing reminder")
                    for event in due:
                        await self.store.mark_reminded(event.id)
                        for cb in self._callbacks:
                            try:
                                await cb(event)
                            except Exception as e:
                                logger.error(f"Reminder callback error: {e}")
            except Exception as e:
                logger.error(f"Reminder check error: {e}")

            await asyncio.sleep(self.interval)
