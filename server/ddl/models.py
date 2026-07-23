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


@dataclass
class DDLItem:
    """Unified DDL event entry."""
    title: str
    course: str = ""
    type: str = "自定义"       # "作业", "考试", "实验", "自定义"
    source: str = "manual"     # "zju", "pta", "manual"
    deadline: datetime = field(default_factory=lambda: datetime.now(CST))
    advance_minutes: int = 1440  # notify N minutes before deadline
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

        created = d.get("created_at", "")
        if created:
            try:
                created = datetime.fromisoformat(created)
            except (ValueError, TypeError):
                created = datetime.now(CST)

        return cls(
            id=d.get("id", str(uuid4())),
            title=d.get("title", ""),
            course=d.get("course", ""),
            type=d.get("type", "自定义"),
            source=d.get("source", "manual"),
            deadline=deadline,
            advance_minutes=d.get("advance_minutes", 1440),
            url=d.get("url", ""),
            rate=d.get("rate"),
            status=d.get("status", "pending"),
            reminder_sent=d.get("reminder_sent", False),
            created_at=created,
        )
