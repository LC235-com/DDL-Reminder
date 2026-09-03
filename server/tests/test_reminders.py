import os
import sys
import tempfile
import unittest
import json
import types
from unittest.mock import patch
from datetime import datetime, timedelta, timezone

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))

from ddl.models import CST, DEFAULT_REMINDER_MINUTES, DDLItem
from ddl.scheduler import ReminderScheduler
from ddl.store import EventStore
from llm.intent_parser import IntentParser
import server as server_module
from server import ClientState, _mutation_tool_hint, _send_speech


class ReminderModelTests(unittest.TestCase):
    def test_explicit_mutation_keywords_route_to_write_tools(self):
        self.assertEqual(_mutation_tool_hint("把高数作业改到明天晚上八点"), "modify_reminder")
        self.assertEqual(_mutation_tool_hint("删除数据结构作业"), "delete_reminder")
        self.assertEqual(_mutation_tool_hint("我已经完成实验报告"), "mark_done")
        self.assertEqual(_mutation_tool_hint("提醒我周五交论文"), "add_reminder")

    def test_new_item_has_requested_default_schedule(self):
        deadline = datetime(2026, 9, 2, 20, 0, tzinfo=CST)
        item = DDLItem("报告", deadline=deadline)
        self.assertEqual(item.reminder_minutes, DEFAULT_REMINDER_MINUTES)
        schedule = dict(item.reminder_schedule())
        self.assertEqual(schedule["before:180"], deadline - timedelta(hours=3))
        # Due-day 08:00 becomes 12 hours before this 20:00 deadline.
        self.assertEqual(schedule["before:720"], datetime(2026, 9, 2, 8, 0, tzinfo=CST))
        self.assertFalse(item.to_dict()["remind_at_day_start"])

    def test_legacy_record_keeps_its_single_custom_reminder(self):
        item = DDLItem.from_dict({
            "title": "旧任务",
            "deadline": "2026-09-02T20:00:00+08:00",
            "advance_minutes": 90,
        })
        self.assertEqual(item.reminder_minutes, [90])


class ReminderSchedulerTests(unittest.IsolatedAsyncioTestCase):
    async def test_search_matches_course_and_title_as_one_spoken_name(self):
        with tempfile.TemporaryDirectory() as directory:
            store = EventStore(os.path.join(directory, "events.json"))
            item = DDLItem(
                "选课截止时间", course="日语二",
                deadline=datetime.now(timezone.utc) + timedelta(days=5),
            )
            await store.add(item)

            spaced = await store.search("日语二 选课截止")
            spoken = await store.search("日语二的选课截止我已经完成了帮我划掉它")

            self.assertEqual([event.id for event in spaced], [item.id])
            self.assertEqual([event.id for event in spoken], [item.id])

    async def test_search_tolerates_small_asr_error(self):
        with tempfile.TemporaryDirectory() as directory:
            store = EventStore(os.path.join(directory, "events.json"))
            item = DDLItem(
                "复变函数与拉普拉斯变换",
                deadline=datetime.now(timezone.utc) + timedelta(days=5),
            )
            await store.add(item)
            matches = await store.search("负变函数与拉普拉斯变换")
            self.assertEqual([event.id for event in matches], [item.id])

    async def test_cross_field_modify_updates_instead_of_creating_duplicate(self):
        with tempfile.TemporaryDirectory() as directory:
            store = EventStore(os.path.join(directory, "events.json"))
            parser = IntentParser(store)
            item = DDLItem(
                "选课截止时间", course="日语二",
                deadline=datetime(2099, 9, 8, 17, 0, tzinfo=timezone.utc),
            )
            await store.add(item)

            result = await parser._modify_reminder({
                "title_keyword": "日语二选课截止",
                "new_time": "2099-09-08 17:30",
            })

            pending = await store.get_pending()
            self.assertIn("已修改", result)
            self.assertEqual(len(pending), 1)
            self.assertEqual(pending[0].id, item.id)

    async def test_tts_uses_explicit_messages_and_never_ping_as_end_marker(self):
        class FakeWebSocket:
            def __init__(self):
                self.sent = []

            async def send(self, payload):
                self.sent.append(payload)

        websocket = FakeWebSocket()
        state = ClientState()
        await _send_speech(websocket, state, "测试语音", "happy", b"123456")

        self.assertEqual(json.loads(websocket.sent[0])["cmd"], "speak")
        start = json.loads(websocket.sent[1])
        end = json.loads(websocket.sent[-1])
        self.assertEqual(start["cmd"], "audio_stream_start")
        self.assertEqual(end["cmd"], "audio_stream_end")
        self.assertEqual(start["stream_id"], end["stream_id"])
        self.assertEqual(websocket.sent[2], b"123456")

    async def test_completion_has_deterministic_fallback_when_llm_returns_only_prose(self):
        class ProseOnlyLLM:
            async def chat(self, messages, tools=None, tool_choice=None):
                return {"response": "已经完成", "tool_calls": [], "emotion": "happy"}

        with tempfile.TemporaryDirectory() as directory:
            test_store = EventStore(os.path.join(directory, "events.json"))
            item = DDLItem(
                "选课截止时间", course="日语二",
                deadline=datetime.now(timezone.utc) + timedelta(days=5),
            )
            await test_store.add(item)
            previous_store = server_module.store
            previous_llm = server_module.llm_module
            try:
                server_module.store = test_store
                server_module.llm_module = ProseOnlyLLM()
                tool_module = types.ModuleType("llm.openai_compatible")
                tool_module.DDL_TOOLS = [{
                    "type": "function",
                    "function": {"name": "mark_done", "parameters": {"type": "object"}},
                }]
                with patch.dict(sys.modules, {"llm.openai_compatible": tool_module}):
                    result, expected = await server_module._repair_tool_routing(
                        "日语二的选课截止我已经完成了帮我划掉它",
                        [{"role": "user", "content": "完成日语二选课截止"}],
                        {"response": "已经完成", "tool_calls": [], "emotion": "happy"},
                    )
            finally:
                server_module.store = previous_store
                server_module.llm_module = previous_llm

            self.assertEqual(expected, "mark_done")
            self.assertEqual(result["tool_calls"][0]["function"]["name"], "mark_done")
            args = json.loads(result["tool_calls"][0]["function"]["arguments"])
            self.assertEqual(args["title_keyword"], "日语二 选课截止时间")

    async def test_scheduler_finds_unsent_due_occurrence(self):
        with tempfile.TemporaryDirectory() as directory:
            store = EventStore(os.path.join(directory, "events.json"))
            deadline = datetime.now(timezone.utc) + timedelta(hours=2)
            item = DDLItem("测试", deadline=deadline, reminder_minutes=[180, 60], remind_at_day_start=False)
            await store.add(item)
            scheduler = ReminderScheduler(store)
            due = await scheduler.check_now()
            self.assertEqual([event.id for event in due], [item.id])
            await store.mark_reminded(item.id, ["before:180"])
            self.assertEqual(await scheduler.check_now(), [])

    async def test_pending_excludes_expired_and_done(self):
        with tempfile.TemporaryDirectory() as directory:
            store = EventStore(os.path.join(directory, "events.json"))
            future = DDLItem("未来", deadline=datetime.now(timezone.utc) + timedelta(days=1))
            expired = DDLItem("过期", deadline=datetime.now(timezone.utc) - timedelta(minutes=1))
            done = DDLItem("完成", deadline=datetime.now(timezone.utc) + timedelta(days=1), status="done")
            await store.add(future); await store.add(expired); await store.add(done)
            self.assertEqual([item.title for item in await store.get_pending()], ["未来"])

    async def test_cleanup_keeps_recent_records_and_removes_two_week_old_records(self):
        with tempfile.TemporaryDirectory() as directory:
            store = EventStore(os.path.join(directory, "events.json"))
            now = datetime.now(timezone.utc)
            recent_done = DDLItem("近期完成", deadline=now - timedelta(days=7), status="done")
            old_done = DDLItem("很久前完成", deadline=now - timedelta(days=15), status="done")
            old_expired = DDLItem("很久前过期", deadline=now - timedelta(days=15))
            future = DDLItem("未来", deadline=now + timedelta(days=1))
            for item in (recent_done, old_done, old_expired, future):
                await store.add(item)

            removed = await store.cleanup_old(expired_days=14, done_days=14)

            self.assertEqual(removed, 2)
            self.assertEqual(
                {item.title for item in await store.get_all()},
                {"近期完成", "未来"},
            )

    async def test_voice_modify_updates_or_creates_and_delete_hides(self):
        with tempfile.TemporaryDirectory() as directory:
            store = EventStore(os.path.join(directory, "events.json"))
            parser = IntentParser(store)
            await parser._modify_reminder({
                "title_keyword": "高数作业", "new_time": "2099-09-03 20:00"
            })
            items = await store.get_pending()
            self.assertEqual(len(items), 1)
            original_id = items[0].id
            await parser._modify_reminder({
                "title_keyword": "高数", "new_time": "2099-09-04 21:30"
            })
            items = await store.get_pending()
            self.assertEqual(len(items), 1)
            self.assertEqual(items[0].id, original_id)
            self.assertIn("2099-09-04T21:30", items[0].deadline_iso())
            await parser._add_reminder({
                "title": "高数作业", "time": "2099-09-05 22:00"
            })
            items = await store.get_pending()
            self.assertEqual(len(items), 1)
            self.assertEqual(items[0].id, original_id)
            self.assertIn("2099-09-05T22:00", items[0].deadline_iso())
            await parser._mark_done({"title_keyword": "高数"})
            self.assertEqual(await store.get_pending(), [])

    async def test_detailed_tool_report_reflects_real_store_result(self):
        with tempfile.TemporaryDirectory() as directory:
            store = EventStore(os.path.join(directory, "events.json"))
            parser = IntentParser(store)
            item = DDLItem("实验报告", deadline=datetime.now(timezone.utc) + timedelta(days=2))
            await store.add(item)

            text, reports = await parser.execute_detailed([{
                "id": "call_modify",
                "function": {
                    "name": "modify_reminder",
                    "arguments": '{"title_keyword":"实验报告","new_time":"2099-09-05 18:30"}',
                },
            }])
            self.assertTrue(reports[0]["success"])
            self.assertEqual(reports[0]["tool"], "modify_reminder")
            self.assertIn("已修改", text)

            _, failed = await parser.execute_detailed([{
                "id": "call_bad_modify",
                "function": {"name": "modify_reminder", "arguments": "{}"},
            }])
            self.assertFalse(failed[0]["success"])


if __name__ == "__main__":
    unittest.main()
