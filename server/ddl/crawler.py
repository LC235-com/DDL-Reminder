"""
Unified crawler wrapper — periodically fetches DDLs from all configured sources.

Supports ZJU (学在浙大) and PTA (拼题A) with extensible interface.
Deduplicates by (title, course, deadline) when merging into the event store.
"""

import asyncio
import logging
import os
import sys
from datetime import datetime, timezone, timedelta

from .models import DDLItem
from .store import EventStore

logger = logging.getLogger(__name__)

CST = timezone(timedelta(hours=8))


class CrawlerScheduler:
    """Manages periodic DDL crawling from multiple sources."""

    def __init__(self, store: EventStore, interval: int = 1800):
        """
        Args:
            store: EventStore instance
            interval: Crawl interval in seconds (default 1800 = 30 min)
        """
        self.store = store
        self.interval = interval
        self._task: asyncio.Task | None = None
        self._running = False
        self._on_new_events: list = []  # callbacks when new events found

    def on_new(self, callback):
        """Register callback: async def callback(new_events: list[DDLItem])."""
        self._on_new_events.append(callback)

    async def start(self):
        """Start periodic crawling."""
        self._running = True
        self._task = asyncio.create_task(self._loop())
        logger.info(f"Crawler scheduler started (interval={self.interval}s)")

    async def stop(self):
        """Stop crawling."""
        self._running = False
        if self._task:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass

    async def crawl_once(self) -> list[DDLItem]:
        """
        Run one crawl cycle across all enabled sources.
        Returns newly discovered events.
        """
        new_events: list[DDLItem] = []

        # ── ZJU (学在浙大) ──
        if os.environ.get("ZJU_USER"):
            try:
                zju = ZJUCrawler()
                items = await zju.fetch()
                logger.info(f"ZJU: got {len(items)} items")
                new_events.extend(items)
            except Exception as e:
                logger.error(f"ZJU crawl failed: {e}")

        # ── PTA (拼题A) ──
        if os.environ.get("PTA_COOKIES"):
            try:
                pta = PTACrawler()
                items = await pta.fetch()
                logger.info(f"PTA: got {len(items)} items")
                new_events.extend(items)
            except Exception as e:
                logger.error(f"PTA crawl failed: {e}")

        # ── Merge into store (dedup) ──
        merged: list[DDLItem] = []
        for event in new_events:
            _, is_new = await self.store.upsert(event)
            if is_new:
                merged.append(event)

        if merged:
            logger.info(f"Crawl: {len(merged)} new events merged")

        return merged

    async def _loop(self):
        """Main loop — crawl immediately, then every `interval` seconds."""
        while self._running:
            try:
                new_events = await self.crawl_once()
                if new_events:
                    for cb in self._on_new_events:
                        try:
                            await cb(new_events)
                        except Exception as e:
                            logger.error(f"New events callback error: {e}")
            except Exception as e:
                logger.error(f"Crawl loop error: {e}")

            await asyncio.sleep(self.interval)


# ── Individual Crawlers ───────────────────────────────────────

class ZJUCrawler:
    """
    学在浙大 crawler — uses Playwright to log in and fetch assignments.

    Requires ZJU_USER and ZJU_PASS environment variables.
    Adapted from zju-ddl-killer/ZJU-DDL-Scraper.
    """

    def __init__(self):
        self.username = os.environ.get("ZJU_USER", "")
        self.password = os.environ.get("ZJU_PASS", "")

    async def fetch(self) -> list[DDLItem]:
        """Fetch DDL items from 学在浙大."""
        if not self.username or not self.password:
            return []

        try:
            data = await self._login_and_fetch()
        except Exception as e:
            logger.error(f"ZJU fetch failed: {e}")
            return []

        ddls = []
        seen = set()

        for todo in data.get("todo_list", []):
            end = todo.get("end_time")
            if not end:
                continue
            try:
                dl = datetime.fromisoformat(end.replace("Z", "+00:00"))
            except (ValueError, TypeError):
                continue

            title = todo.get("title", "未知")
            course = todo.get("course_name", "未知")
            course_id = todo.get("course_id", "")

            key = (title, course, dl.isoformat())
            if key in seen:
                continue
            seen.add(key)

            ddls.append(DDLItem(
                title=title,
                course=course,
                type="作业",
                source="zju",
                deadline=dl,
                url=f"https://courses.zju.edu.cn/course/{course_id}" if course_id else "",
                rate=todo.get("submit_rate"),
            ))

        ddls.sort(key=lambda x: x.deadline if not isinstance(x.deadline, str) else datetime.now(CST))
        return ddls

    async def _login_and_fetch(self) -> dict:
        """Use Playwright to log in and get todo list."""
        from playwright.async_api import async_playwright
        import json

        async with async_playwright() as p:
            browser = await p.chromium.launch(headless=True)
            ctx = await browser.new_context(
                user_agent="Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36"
            )
            page = await ctx.new_page()

            # CAS login
            await page.goto("https://zjuam.zju.edu.cn/cas/login", wait_until="load")
            await page.fill("#username", self.username)
            await page.fill("#password", self.password)
            await page.click("#dl")
            await asyncio.sleep(3)

            # Navigate to courses
            await page.goto("https://course.zju.edu.cn/learninginzju?locale=en-US", wait_until="load")
            await asyncio.sleep(2)
            cas_btn = await page.query_selector("text=CAS Login")
            if cas_btn:
                await cas_btn.click()
                await asyncio.sleep(5)

            # User center
            await page.goto("https://courses.zju.edu.cn/user/index#/", wait_until="load")
            await asyncio.sleep(3)

            # Get API data
            await page.goto("https://courses.zju.edu.cn/api/todos?no-intercept=true", wait_until="load")
            await asyncio.sleep(2)

            raw_text = await page.inner_text("pre")
            await browser.close()

        return json.loads(raw_text)


class PTACrawler:
    """
    PTA (拼题A) crawler — uses REST API with cookie auth.

    Requires PTA_COOKIES environment variable.
    Adapted from zju-ddl-killer/ZJU-DDL-Scraper.
    """

    BASE_URL = "https://pintia.cn"
    API_PROBLEM_SETS = "/api/problem-sets"

    def __init__(self):
        self.cookie_str = os.environ.get("PTA_COOKIES", "")
        self.timeout = 30

    async def fetch(self) -> list[DDLItem]:
        """Fetch DDL items from PTA."""
        if not self.cookie_str:
            return []

        try:
            items = await asyncio.to_thread(self._fetch_all)
        except Exception as e:
            logger.error(f"PTA fetch failed: {e}")
            return []

        from dateutil.parser import isoparse
        ddls = []
        for item in items:
            end_str = item.get("endAt")
            if not end_str:
                continue
            try:
                deadline = isoparse(end_str)
            except (ValueError, TypeError):
                continue

            name = item.get("name", "")
            teacher = item.get("ownerNickname", "")
            school = item.get("organizationName", "")

            ddls.append(DDLItem(
                title=name,
                course=name.split("_")[0] if "_" in name else school,
                type="作业",
                source="pta",
                deadline=deadline,
                url=f"https://pintia.cn/problem-sets/{item.get('id', '')}",
                rate=None,
            ))

        ddls.sort(key=lambda x: x.deadline if not isinstance(x.deadline, str) else datetime.now(CST))
        return ddls

    def _fetch_all(self) -> list[dict]:
        import requests
        import time

        session = requests.Session()
        session.headers.update({
            "User-Agent": (
                "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                "AppleWebKit/537.36 Chrome/120.0.0.0 Safari/537.36"
            ),
            "Accept": "application/json",
            "Origin": self.BASE_URL,
            "Referer": f"{self.BASE_URL}/problem-sets",
        })

        for item in self.cookie_str.split(";"):
            item = item.strip()
            if "=" in item:
                key, value = item.split("=", 1)
                session.cookies.set(key.strip(), value.strip(), domain=".pintia.cn")

        all_items = []
        total = None
        limit = 50
        page = 0

        while total is None or len(all_items) < total:
            resp = session.get(
                f"{self.BASE_URL}{self.API_PROBLEM_SETS}",
                params={"page": page, "limit": limit},
                timeout=self.timeout,
            )
            data = resp.json()
            if total is None:
                total = data.get("total", 0)
            items = data.get("problemSets", [])
            all_items.extend(items)
            page += 1
            if len(items) < limit:
                break
            time.sleep(0.3)

        return all_items
