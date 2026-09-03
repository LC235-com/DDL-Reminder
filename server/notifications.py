"""Optional phone-facing reminder channels (SMTP email and DingTalk robot)."""

from __future__ import annotations

import asyncio
import base64
import hashlib
import hmac
import json
import logging
import smtplib
import ssl
import time
from email.message import EmailMessage
from urllib.parse import quote_plus
from urllib.request import Request, urlopen

import config

logger = logging.getLogger(__name__)


def _send_email(subject: str, content: str) -> None:
    recipients = [item.strip() for item in config.NOTIFY_EMAIL_TO.split(",") if item.strip()]
    if not recipients:
        return
    message = EmailMessage()
    message["Subject"] = subject
    message["From"] = config.SMTP_FROM or config.SMTP_USER
    message["To"] = ", ".join(recipients)
    message.set_content(content)

    if config.SMTP_SSL:
        client = smtplib.SMTP_SSL(config.SMTP_HOST, config.SMTP_PORT, timeout=10,
                                  context=ssl.create_default_context())
    else:
        client = smtplib.SMTP(config.SMTP_HOST, config.SMTP_PORT, timeout=10)
        client.starttls(context=ssl.create_default_context())
    try:
        if config.SMTP_USER:
            client.login(config.SMTP_USER, config.SMTP_PASSWORD)
        client.send_message(message)
    finally:
        client.quit()


def _send_dingtalk(content: str) -> None:
    webhook = config.DINGTALK_WEBHOOK
    if not webhook:
        return
    if config.DINGTALK_SECRET:
        timestamp = str(round(time.time() * 1000))
        digest = hmac.new(
            config.DINGTALK_SECRET.encode(),
            f"{timestamp}\n{config.DINGTALK_SECRET}".encode(),
            digestmod=hashlib.sha256,
        ).digest()
        separator = "&" if "?" in webhook else "?"
        webhook += f"{separator}timestamp={timestamp}&sign={quote_plus(base64.b64encode(digest))}"
    payload = json.dumps({"msgtype": "text", "text": {"content": content}}, ensure_ascii=False).encode()
    request = Request(webhook, data=payload, headers={"Content-Type": "application/json"}, method="POST")
    with urlopen(request, timeout=10) as response:
        result = json.loads(response.read().decode("utf-8"))
    if result.get("errcode", 0) != 0:
        raise RuntimeError(f"DingTalk error: {result}")


async def send_mobile_notification(subject: str, content: str) -> bool:
    """Send all configured channels independently; one failure won't block another."""
    jobs = []
    if config.NOTIFY_EMAIL_TO and config.SMTP_HOST:
        jobs.append(asyncio.to_thread(_send_email, subject, content))
    if config.DINGTALK_WEBHOOK:
        jobs.append(asyncio.to_thread(_send_dingtalk, content))
    if not jobs:
        return False
    results = await asyncio.gather(*jobs, return_exceptions=True)
    delivered = False
    for result in results:
        if isinstance(result, Exception):
            logger.error("Mobile notification failed: %s", result)
        else:
            delivered = True
    return delivered
