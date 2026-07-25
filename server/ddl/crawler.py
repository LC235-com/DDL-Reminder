"""
Unified crawler wrapper — periodically fetches DDLs from all configured sources.

Supports ZJU (学在浙大) and PTA (拼题A) with extensible interface.
Deduplicates by (title, course, deadline) when merging into the event store.
"""

import asyncio
import json
import logging
import os
import sys
from datetime import datetime, timezone, timedelta
from pathlib import Path

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

        # Report which sources are configured
        sources = []
        if os.environ.get("ZJU_USER") and os.environ.get("ZJU_PASS"):
            sources.append("ZJU-教务网(考试)")
            sources.append("ZJU-学在浙大(作业)")
        if os.environ.get("PTA_COOKIES"):
            sources.append("PTA")
        sources.append("Local")
        logger.info(f"Crawler scheduler started: {', '.join(sources)} "
                     f"(interval={self.interval}s)")
        if len(sources) == 1:
            logger.warning("Only Local crawler active. Set ZJU_USER/ZJU_PASS or "
                           "PTA_COOKIES env vars for cloud sources, or add items "
                           "to server/data/manual_ddl.json")

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

        # ── ZJU Sources (require ZJU_USER + ZJU_PASS) ──
        zju_user = os.environ.get("ZJU_USER", "")
        zju_pass = os.environ.get("ZJU_PASS", "")

        if zju_user and zju_pass:
            # Single Playwright session — all cookies accumulate naturally
            # (matching Celechron's single HttpClient approach)
            def crawl_zju_all():
                z = ZjuSession(zju_user, zju_pass)
                return z.crawl_all()

            try:
                result = await asyncio.to_thread(crawl_zju_all)
                new_events.extend(result)
            except Exception as e:
                logger.error(f"ZJU crawl failed: {e}", exc_info=True)
        else:
            logger.debug("ZJU crawlers skipped (ZJU_USER + ZJU_PASS not set)")

        # ── PTA (拼题A) ──
        if os.environ.get("PTA_COOKIES"):
            try:
                pta = PTACrawler()
                items = await pta.fetch()
                logger.info(f"PTA: got {len(items)} items")
                new_events.extend(items)
            except Exception as e:
                logger.error(f"PTA crawl failed: {e}", exc_info=True)
        else:
            logger.debug("PTA crawler skipped (PTA_COOKIES not set)")

        # ── Local (manual DDL file, always runs) ──
        try:
            local = LocalCrawler()
            items = await local.fetch()
            if items:
                logger.info(f"Local: got {len(items)} items")
                new_events.extend(items)
        except Exception as e:
            logger.debug(f"Local crawl: {e}")

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

class ZjuSession:
    """
    Unified ZJU crawler — ONE Playwright browser context for everything.

    Matches Celechron's architecture: a single HttpClient carries all cookies
    through CAS login → zdbk SSO → zdbk API → courses SSO → courses API.
    Cookies accumulate naturally on the BrowserContext — no manual transfer.

    Usage:
        session = ZjuSession(username, password)
        events = session.crawl_all()  # returns list[DDLItem]
    """

    CAS_LOGIN = "https://zjuam.zju.edu.cn/cas/login"
    ZDBK_SSO = ("https://zjuam.zju.edu.cn/cas/login"
                "?service=https%3A%2F%2Fzdbk.zju.edu.cn%2Fjwglxt%2Fxtgl%2Flogin_ssologin.html")
    ZDBK_API = ("https://zdbk.zju.edu.cn/jwglxt/xskscx/kscx_cxXsgrksIndex.html"
                "?doType=query&queryModel.showCount=5000")
    ZDBK_HOME = "https://zdbk.zju.edu.cn/jwglxt/xtgl/index_initMenu.html"
    COURSES_HOME = "https://courses.zju.edu.cn/user/index"
    COURSES_API = "https://courses.zju.edu.cn/api/todos"

    UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
          "AppleWebKit/537.36 (KHTML, like Gecko) "
          "Chrome/122.0.0.0 Safari/537.36")

    def __init__(self, username: str, password: str):
        self.username = username
        self.password = password

    def crawl_all(self) -> list:
        """
        Run full crawl in ONE browser session.
        All cookies accumulate naturally on the BrowserContext.
        """
        from playwright.sync_api import sync_playwright, TimeoutError as PwTimeout

        events: list = []

        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True)
            ctx = browser.new_context(
                user_agent=self.UA,
                locale="zh-CN",
                ignore_https_errors=True,
            )
            page = ctx.new_page()

            try:
                # ── Step 1: CAS login ──
                logger.info("ZJU: CAS login...")
                page.goto(self.CAS_LOGIN, wait_until="networkidle")
                page.wait_for_timeout(1000)

                if "zjuam" not in page.url:
                    logger.info(f"ZJU: already logged in (url={page.url[:60]})")
                else:
                    page.fill("#username", self.username)
                    page.fill("#password", self.password)
                    page.click("#dl")
                    page.wait_for_timeout(3000)

                    if "密码错误" in (page.inner_text("body")[:500] or ""):
                        logger.error("ZJU: wrong password")
                        return []
                    if "zjuam" in page.url:
                        logger.error(f"ZJU: CAS login failed, url={page.url[:80]}")
                        return []

                sso = None
                for c in ctx.cookies():
                    if c["name"] == "iPlanetDirectoryPro":
                        sso = c["value"]
                        break
                if not sso:
                    logger.error("ZJU: no iPlanetDirectoryPro after CAS login")
                    return []
                logger.info("ZJU: CAS login OK")

                # ── Step 2: 教务网 (zdbk) exams ──
                logger.info("ZJU: zdbk SSO...")
                try:
                    page.goto(self.ZDBK_SSO, wait_until="networkidle")
                    page.wait_for_timeout(2000)
                except PwTimeout:
                    logger.warning("ZJU: zdbk SSO timeout, continuing...")

                jsessionid = None
                for c in ctx.cookies():
                    if c["name"] == "JSESSIONID":
                        jsessionid = c["value"]
                        break

                if jsessionid:
                    logger.info("ZJU: zdbk SSO OK, fetching exams...")
                    try:
                        page.goto(self.ZDBK_HOME, wait_until="networkidle")
                        page.wait_for_timeout(500)
                    except PwTimeout:
                        pass

                    api_ctx = ctx.request
                    resp = api_ctx.post(self.ZDBK_API, headers={
                        "Accept": "application/json, text/javascript, */*; q=0.01",
                        "X-Requested-With": "XMLHttpRequest",
                        "Referer": self.ZDBK_HOME,
                    })
                    logger.info(f"ZJU: zdbk API HTTP {resp.status}")

                    if resp.status == 200 and resp.text().strip():
                        data = json.loads(resp.text())
                        items = data.get("items", [])
                        logger.info(f"ZJU: {len(items)} exam entries")

                        # Debug: log first item to verify field names
                        if items and len(items) > 0:
                            first = {k: str(v)[:80] for k, v in items[0].items()
                                     if isinstance(items[0], dict)}
                            logger.info(f"ZJU: sample item keys={list(first.keys())}")
                            logger.info(f"ZJU: sample item: {first}")

                        now = datetime.now(CST)
                        seen = set()
                        has_time = 0
                        past_count = 0
                        for item in items:
                            if not isinstance(item, dict):
                                continue
                            cn = item.get("kcmc", "未知课程")
                            for time_key, exam_type in [("kssj", "期末考试"), ("qzkssj", "期中考试")]:
                                ts = (item.get(time_key) or "").strip()
                                if not ts:
                                    continue
                                has_time += 1
                                dt = self._parse_exam_time(ts)
                                if dt is None:
                                    continue
                                if dt < now:
                                    past_count += 1
                                    continue
                                k = (cn, exam_type, dt.isoformat())
                                if k in seen:
                                    continue
                                seen.add(k)
                                events.append(DDLItem(
                                    title=f"{cn} {exam_type}",
                                    course=cn,
                                    type=exam_type,
                                    source="zju_zdbk",
                                    deadline=dt,
                                    advance_minutes=2880,
                                ))
                        logger.info(f"ZJU: {has_time} exam times found, "
                                    f"{past_count} past, "
                                    f"{len(events)} future exams after zdbk")
                    else:
                        logger.warning(f"ZJU: zdbk API returned HTTP {resp.status}")
                else:
                    logger.warning("ZJU: no JSESSIONID, skipping zdbk exams")

                # ── Step 3: 学在浙大 (courses) todos ──
                logger.info("ZJU: courses SSO...")
                try:
                    page.goto(self.COURSES_HOME, wait_until="networkidle")
                    page.wait_for_timeout(2000)
                except PwTimeout:
                    logger.warning("ZJU: courses SSO timeout, continuing...")

                session_cookie = None
                for c in ctx.cookies():
                    if c["name"] == "session" and c["value"]:
                        session_cookie = c["value"]
                        break

                if session_cookie:
                    logger.info("ZJU: courses SSO OK, fetching todos...")
                    api_ctx = ctx.request
                    resp = api_ctx.get(self.COURSES_API)
                    logger.info(f"ZJU: courses API HTTP {resp.status}")

                    if resp.status == 200 and resp.text().strip():
                        data = json.loads(resp.text())
                        todo_list = data if isinstance(data, list) else data.get("todo_list", [])
                        logger.info(f"ZJU: {len(todo_list)} raw todos")

                        now = datetime.now(CST)
                        seen = set()
                        for todo in todo_list:
                            if not isinstance(todo, dict):
                                continue
                            end_str = todo.get("end_time") or todo.get("deadline")
                            if not end_str:
                                continue
                            try:
                                dl = datetime.fromisoformat(end_str.replace("Z", "+00:00"))
                            except (ValueError, TypeError):
                                continue
                            if dl < now:
                                continue
                            title = todo.get("title", "未知作业")
                            course = todo.get("course_name", "未知课程")
                            k = (title, course, dl.isoformat())
                            if k in seen:
                                continue
                            seen.add(k)
                            events.append(DDLItem(
                                title=title,
                                course=course,
                                type=todo.get("type", "作业"),
                                source="zju_courses",
                                deadline=dl,
                                advance_minutes=1440,
                            ))
                        logger.info(f"ZJU: {len(events)} total after courses")
                    else:
                        logger.warning(f"ZJU: courses API returned HTTP {resp.status}")
                else:
                    logger.warning("ZJU: no session cookie, skipping courses todos")

            except Exception as e:
                logger.error(f"ZJU crawl error: {e}", exc_info=True)
            finally:
                browser.close()

        return events

    @staticmethod
    def _parse_exam_time(ts: str) -> datetime | None:
        """
        Parse ZJU exam time format. Examples:
        - "2025年08月23日(14:00-16:40)" → end time 16:40
        - "2026-01-15 08:00" → exact time
        Returns datetime in CST, or None.
        """
        import re
        # Format 1: "2025年08月23日(14:00-16:40)" — Chinese date + time range
        m = re.match(r'(\d{4})年(\d{2})月(\d{2})日\((\d{2}):(\d{2})-(\d{2}):(\d{2})\)', ts)
        if m:
            y, mo, d, h1, mi1, h2, mi2 = [int(x) for x in m.groups()]
            # Use END time as the deadline
            return datetime(y, mo, d, h2, mi2, tzinfo=CST)
        # Format 2: "2026-01-15 08:00" — ISO-like
        m = re.match(r'(\d{4})-(\d{2})-(\d{2})\s+(\d{2}):(\d{2})', ts)
        if m:
            y, mo, d, h, mi = [int(x) for x in m.groups()]
            return datetime(y, mo, d, h, mi, tzinfo=CST)
        logger.debug(f"ZJU: unparseable exam time: {ts[:40]}")
        return None


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


# ── Local / Manual Crawler ─────────────────────────────────────

class LocalCrawler:
    """
    Local file-based DDL source — reads from server/data/manual_ddl.json.

    Always available, no credentials needed. Useful for testing and
    as a fallback when no cloud crawlers are configured.

    JSON format:
    [
        {
            "title": "提交实验报告",
            "course": "数字电路",
            "type": "作业",
            "deadline": "2026-07-30T23:59:00+08:00",
            "advance_minutes": 1440
        }
    ]
    """

    DATA_FILE = Path(__file__).resolve().parent.parent / "data" / "manual_ddl.json"

    async def fetch(self) -> list[DDLItem]:
        """Read DDL items from the local JSON file."""
        if not self.DATA_FILE.exists():
            return []

        try:
            raw = await asyncio.to_thread(self.DATA_FILE.read_text, encoding="utf-8")
            entries = json.loads(raw)
        except (json.JSONDecodeError, OSError) as e:
            logger.warning(f"Local DDL file read failed: {e}")
            return []

        ddls = []
        for entry in entries:
            try:
                deadline = datetime.fromisoformat(entry["deadline"])
            except (ValueError, KeyError):
                continue

            ddls.append(DDLItem(
                title=entry.get("title", "未命名"),
                course=entry.get("course", ""),
                type=entry.get("type", "自定义"),
                source="manual",
                deadline=deadline,
                advance_minutes=entry.get("advance_minutes", 1440),
            ))

        return ddls
