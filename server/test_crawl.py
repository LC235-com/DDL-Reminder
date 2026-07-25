#!/usr/bin/env python3
"""
Test script — run crawlers standalone to verify they work.
Based on Celechron's architecture (single HttpClient = single BrowserContext).

Usage:
    python test_crawl.py              # Test all enabled crawlers
    python test_crawl.py zju          # Test ZJU (CAS → zdbk → courses)
    python test_crawl.py pta          # Test PTA crawler

Environment variables needed:
    ZJU_USER / ZJU_PASS  — 学在浙大 / 教务网 credentials
    PTA_COOKIES          — PTA cookie string
"""

import asyncio, json, logging, os, sys
from datetime import datetime, timezone, timedelta
from pathlib import Path

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    datefmt="%H:%M:%S",
)

CST = timezone(timedelta(hours=8))
sys.path.insert(0, str(Path(__file__).parent))


async def test_zju():
    """Test full ZJU flow: CAS →教务网 exams + 学在浙大 todos (single session)."""
    from ddl.crawler import ZjuSession

    zju_user = os.environ.get("ZJU_USER", "")
    zju_pass = os.environ.get("ZJU_PASS", "")

    if not zju_user or not zju_pass:
        print("ERROR: ZJU_USER / ZJU_PASS not set.")
        print("   set ZJU_USER=你的学号")
        print("   set ZJU_PASS=你的密码")
        return

    print("=" * 50)
    print("ZJU Crawl — single BrowserContext (matching Celechron)")
    print("=" * 50)

    def do_crawl():
        z = ZjuSession(zju_user, zju_pass)
        return z.crawl_all()

    try:
        events = await asyncio.to_thread(do_crawl)
        print(f"\nGot {len(events)} future DDL items from ZJU:\n")
        for item in events:
            print(f"  [{item.type}] {item.course} - {item.title}")
            print(f"  Due: {item.deadline_str()} | {item.duration_str()}")
            print()
        if not events:
            print("  (no future items)")
    except Exception as e:
        print(f"Crawl failed: {e}")
        import traceback; traceback.print_exc()


async def test_pta():
    """Test PTA crawler."""
    print("=" * 50)
    print("Testing PTA (拼题A) crawler...")
    print("=" * 50)
    if not os.environ.get("PTA_COOKIES"):
        print("ERROR: PTA_COOKIES not set.")
        return
    from ddl.crawler import PTACrawler
    crawler = PTACrawler()
    try:
        items = await crawler.fetch()
        print(f"Got {len(items)} DDL items:")
        for item in items[:10]:
            print(f"  {item.title} | Due: {item.deadline_str()}")
    except Exception as e:
        print(f"Crawl failed: {e}")
        import traceback; traceback.print_exc()


async def main():
    target = sys.argv[1] if len(sys.argv) > 1 else "all"
    print(f"DDL Reminder — Crawler Tester")
    print(f"Target: {target}")
    print(f"Time: {datetime.now(CST).strftime('%Y-%m-%d %H:%M:%S %Z')}")
    print()

    if target in ("zju", "all"):
        await test_zju()
    if target in ("pta", "all"):
        if target == "all":
            print("\n")
        await test_pta()

    # Always show local
    print("\n" + "=" * 50)
    print("Local DDL data (manual_ddl.json):")
    print("=" * 50)
    from ddl.crawler import LocalCrawler
    items = await LocalCrawler().fetch()
    print(f"Got {len(items)} local items")
    for item in items:
        print(f"  {item.title} | {item.course} | Due: {item.deadline_str()} | {item.duration_str()}")


if __name__ == "__main__":
    asyncio.run(main())
