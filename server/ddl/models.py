"""
Data models — unified DDL event format.

Adapted from ZJU-DDL-Scraper's DDLItem, extended with:
- UUID-based id
- advance_minutes for reminder scheduling
- status tracking (pending/done/snoozed/dismissed)
- reminder_sent flag
"""

from dataclasses import dataclass, field, asdict
from datetime import datetime, timezone, timedelta
from uuid import uuid4

CST = timezone(timedelta(hours=8))
DEFAULT_REMINDER_MINUTES = [1440, 180, 60, 30, 10]
MAX_REMINDERS = 6
MAX_ADVANCE_MINUTES = 30 * 1440 + 23 * 60 + 59


@dataclass
class DDLItem:
    """Unified DDL event entry."""
    title: str
    course: str = ""
    type: str = "自定义"       # "作业", "考试", "实验", "自定义"
    source: str = "manual"     # "zju", "pta", "manual"
    deadline: datetime = field(default_factory=lambda: datetime.now(CST))
    advance_minutes: int = 1440  # notify N minutes before deadline
    reminder_minutes: list[int] = field(default_factory=lambda: DEFAULT_REMINDER_MINUTES.copy())
    remind_at_day_start: bool = True  # 08:00 CST on the due date
    sent_reminders: list[str] = field(default_factory=list)
    url: str = ""
    rate: int | None = None    # submission rate (ZJU)
    id: str = field(default_factory=lambda: str(uuid4()))
    status: str = "pending"    # "pending", "done", "snoozed", "dismissed"
    reminder_sent: bool = False
    created_at: datetime = field(default_factory=lambda: datetime.now(CST))

    def deadline_str(self) -> str:
        """Human-readable deadline in CST."""
        if isinstance(self.deadline, str):
            return self.deadline
        return self.deadline.astimezone(CST).strftime("%Y-%m-%d %H:%M")

    def deadline_short(self) -> str:
        """Short deadline format."""
        if isinstance(self.deadline, str):
            dt = datetime.fromisoformat(self.deadline)
        else:
            dt = self.deadline
        return dt.astimezone(CST).strftime("%m-%d %H:%M")

    def deadline_iso(self) -> str:
        """ISO format for JSON serialization."""
        if isinstance(self.deadline, str):
            return self.deadline
        return self.deadline.isoformat()

    def minutes_remaining(self) -> int:
        """Minutes until deadline. Negative means overdue."""
        now = datetime.now(CST)
        if isinstance(self.deadline, str):
            dl = datetime.fromisoformat(self.deadline)
        else:
            dl = self.deadline
        if dl.tzinfo is None:
            dl = dl.replace(tzinfo=CST)
        return int((dl - now).total_seconds() / 60)

    def duration_str(self) -> str:
        """Human-readable countdown like '3d 5h 12m left' or '12m left'.
        Omits zero units: never shows '0d' or '0h'."""
        mins = self.minutes_remaining()
        if mins < 0:
            mins = -mins
            d = mins // 1440
            h = (mins % 1440) // 60
            m = mins % 60
            parts = []
            if d > 0: parts.append(f"{d}d")
            if h > 0: parts.append(f"{h}h")
            if m > 0 or not parts: parts.append(f"{m}m")
            return "OVERDUE by " + " ".join(parts)
        if mins == 0:
            return "due now"
        d = mins // 1440
        h = (mins % 1440) // 60
        m = mins % 60
        parts = []
        if d > 0: parts.append(f"{d}d")
        if h > 0: parts.append(f"{h}h")
        if m > 0 or not parts: parts.append(f"{m}m")
        return " ".join(parts) + " left"

    def advance_str(self) -> str:
        """Human-readable advance time like '3d before' or '12h before'."""
        adv = self.advance_minutes
        if adv >= 1440:
            d = adv // 1440
            h = (adv % 1440) // 60
            if h > 0:
                return f"Remind: {d}d {h}h before"
            return f"Remind: {d}d before"
        elif adv >= 60:
            h = adv // 60
            m = adv % 60
            if m > 0:
                return f"Remind: {h}h {m}m before"
            return f"Remind: {h}h before"
        else:
            return f"Remind: {adv}m before"

    def reminder_schedule(self) -> list[tuple[str, datetime]]:
        """Return the de-duplicated effective schedule, sorted chronologically.

        The legacy "08:00 on due day" option is converted to an equivalent
        advance-minute value. This makes it editable by the same day/hour/minute
        UI as every other reminder and naturally merges overlapping times.
        """
        deadline = datetime.fromisoformat(self.deadline) if isinstance(self.deadline, str) else self.deadline
        if deadline.tzinfo is None:
            deadline = deadline.replace(tzinfo=CST)
        minutes_set = {
            min(MAX_ADVANCE_MINUTES, max(0, int(minutes)))
            for minutes in self.reminder_minutes
        }
        morning = deadline.astimezone(CST).replace(hour=8, minute=0, second=0, microsecond=0)
        if self.remind_at_day_start and morning < deadline:
            morning_minutes = int((deadline - morning).total_seconds() // 60)
            minutes_set.add(min(MAX_ADVANCE_MINUTES, morning_minutes))
        # Keep the six earliest configured notifications (largest advance values).
        effective_minutes = sorted(minutes_set, reverse=True)[:MAX_REMINDERS]
        schedule = [
            (f"before:{minutes}", deadline - timedelta(minutes=minutes))
            for minutes in effective_minutes
        ]
        return sorted(schedule, key=lambda item: item[1])

    def effective_reminder_minutes(self) -> list[int]:
        """Reminder offsets after converting/merging the legacy morning entry."""
        deadline = datetime.fromisoformat(self.deadline) if isinstance(self.deadline, str) else self.deadline
        if deadline.tzinfo is None:
            deadline = deadline.replace(tzinfo=CST)
        values = {
            min(MAX_ADVANCE_MINUTES, max(0, int(value)))
            for value in self.reminder_minutes
        }
        morning = deadline.astimezone(CST).replace(hour=8, minute=0, second=0, microsecond=0)
        if self.remind_at_day_start and morning < deadline:
            values.add(min(MAX_ADVANCE_MINUTES, int((deadline - morning).total_seconds() // 60)))
        return sorted(values, reverse=True)[:MAX_REMINDERS]

    def tag(self) -> str:
        """Urgency tag emoji."""
        mins = self.minutes_remaining()
        if mins < 0:
            return "⚠️"
        if mins <= 60:
            return "🔥"
        if mins <= 180:
            return "⚡"
        if mins <= 1440:  # 24 hours
            return "📌"
        return "✅"

    def to_dict(self) -> dict:
        """Serialize to JSON-compatible dict."""
        return {
            "id": self.id,
            "title": self.title,
            "course": self.course,
            "type": self.type,
            "source": self.source,
            "deadline": self.deadline_iso(),
            "advance_minutes": self.advance_minutes,
            "reminder_minutes": self.effective_reminder_minutes(),
            # The old special case has now been normalized into reminder_minutes.
            "remind_at_day_start": False,
            "sent_reminders": self.sent_reminders,
            "url": self.url,
            "rate": self.rate,
            "status": self.status,
            "reminder_sent": self.reminder_sent,
            "created_at": self.created_at.isoformat() if not isinstance(self.created_at, str) else self.created_at,
            "tag": self.tag(),
        }

    @classmethod
    def from_dict(cls, d: dict) -> "DDLItem":
        """Deserialize from dict."""
        deadline = d.get("deadline", "")
        if deadline:
            try:
                deadline = datetime.fromisoformat(deadline)
            except (ValueError, TypeError):
                deadline = datetime.now(CST)
        else:
            deadline = datetime.now(CST)

        created = d.get("created_at", "")
        if created:
            try:
                created = datetime.fromisoformat(created)
            except (ValueError, TypeError):
                created = datetime.now(CST)
        else:
            created = datetime.now(CST)

        legacy_advance = int(d.get("advance_minutes", 1440))
        reminder_minutes = d.get("reminder_minutes")
        if reminder_minutes is None:
            # Preserve the behaviour of existing records; newly constructed items
            # receive the new multi-reminder defaults from the dataclass factory.
            reminder_minutes = [legacy_advance]
        reminder_minutes = sorted({
            min(MAX_ADVANCE_MINUTES, max(0, int(v))) for v in reminder_minutes
        }, reverse=True)[:MAX_REMINDERS]

        return cls(
            id=d.get("id", str(uuid4())),
            title=d.get("title", ""),
            course=d.get("course", ""),
            type=d.get("type", "自定义"),
            source=d.get("source", "manual"),
            deadline=deadline,
            advance_minutes=legacy_advance,
            reminder_minutes=reminder_minutes,
            remind_at_day_start=d.get("remind_at_day_start", True),
            sent_reminders=list(d.get("sent_reminders", [])),
            url=d.get("url", ""),
            rate=d.get("rate"),
            status=d.get("status", "pending"),
            reminder_sent=d.get("reminder_sent", False),
            created_at=created,
        )
