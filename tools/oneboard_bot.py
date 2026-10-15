#!/usr/bin/env python3
"""
OneBoard Telegram control bot.

A thin, deterministic bot (no LLM in the loop) that controls a OneBoard CYD
over its HTTP API: /status, /load?id=..., /schema, and the store's tool list.

Run as a systemd user service like the other gateways:
    hermes-gateway-oneboard.service
    (see README section at the bottom of this file)

Env / config:
    ONBOARD_BOT_TOKEN  — Telegram bot token (from BotFather)
    ONBOARD_BOARD_IP   — board IP, e.g. 192.168.1.206
    ONBOARD_STORE_URL  — store base URL, e.g. http://192.168.1.169:8082
                         (used to list available tools)
    ONBOARD_ALLOWED    — comma-separated Telegram user ids allowed to use it
                         (default: none = owner only, from env if set)

INSTALL
------
1. Create the bot in Telegram: message @BotFather -> /newbot -> pick a name and
   a username (e.g. OneBoard Control / OneBoardControlBot). It replies with a
   token like `123456789:AA...`. This token is SECRET — put it in the env file,
   never in chat.
2. Copy the env template and fill it in:
       cp systemd/oneboard-telegram.env.template ~/.hermes/keys/oneboard-telegram.env
       # edit: ONBOARD_BOT_TOKEN, ONBOARD_BOARD_IP, ONBOARD_STORE_URL, ONBOARD_ALLOWED
3. Install the service:
       mkdir -p ~/.config/systemd/user
       cp systemd/hermes-gateway-oneboard.service ~/.config/systemd/user/
       systemctl --user daemon-reload
       systemctl --user enable --now hermes-gateway-oneboard
4. Test: in Telegram, message the bot /status (from the allowed user).
   Logs: journalctl --user -u hermes-gateway-oneboard -f
"""
import os
import sys
import json
import urllib.request
import urllib.parse

from telegram import Update
from telegram.ext import Application, CommandHandler, ContextTypes

TOKEN = os.environ.get("ONBOARD_BOT_TOKEN", "")
BOARD = os.environ.get("ONBOARD_BOARD_IP", "").strip()
STORE = os.environ.get("ONBOARD_STORE_URL", "").strip().rstrip("/")
ALLOWED = {int(x) for x in os.environ.get("ONBOARD_ALLOWED", "").split(",") if x.strip()}


def http_get(url, timeout=8):
    with urllib.request.urlopen(url, timeout=timeout) as resp:
        return resp.read().decode()


def http_post(url, data=None, timeout=8):
    body = urllib.parse.urlencode(data or {}).encode()
    req = urllib.request.Request(url, data=body)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read().decode()


def board_status():
    return json.loads(http_get(f"http://{BOARD}/status"))


def list_tools():
    if not STORE:
        return []
    try:
        return json.loads(http_get(STORE + "/"))["behaviors"]
    except Exception:
        return []


def authorized(update: Update) -> bool:
    if not ALLOWED:
        return True  # not configured -> allow (single-user box)
    return update.effective_user is not None and update.effective_user.id in ALLOWED


async def cmd_help(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    if not authorized(update):
        return
    await update.message.reply_text(
        "OneBoard control\n"
        "/status — current tool + WiFi + IP\n"
        "/tools — list available behaviors\n"
        "/load <id> — load a behavior onto the board\n"
        "/schema — VM vocabulary (self-description)\n"
        "/help — this"
    )


async def cmd_status(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    if not authorized(update):
        return
    try:
        s = board_status()
        await update.message.reply_text(
            f"Tool: {s.get('tool')}\n"
            f"Loaded: {s.get('loaded')}\n"
            f"WiFi: {s.get('wifi')}\n"
            f"IP: {s.get('ip')}\n"
            f"Store: {s.get('store_url')}"
        )
    except Exception as e:
        await update.message.reply_text(f"Board unreachable: {e}")


async def cmd_tools(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    if not authorized(update):
        return
    tools = list_tools()
    if not tools:
        await update.message.reply_text("Store unreachable or empty — check ONBOARD_STORE_URL")
        return
    await update.message.reply_text("Available tools:\n" + "\n".join("• " + t for t in tools))


async def cmd_load(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    if not authorized(update):
        return
    if not ctx.args:
        await update.message.reply_text("Usage: /load <tool-id>")
        return
    tool = ctx.args[0].strip()
    try:
        r = json.loads(http_post(f"http://{BOARD}/load", {"id": tool}))
        if r.get("ok"):
            await update.message.reply_text(f"Loaded {tool} on the board.")
        else:
            await update.message.reply_text(f"Load failed: {r}")
    except Exception as e:
        await update.message.reply_text(f"Board unreachable: {e}")


async def cmd_schema(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    if not authorized(update):
        return
    try:
        s = json.loads(http_get(f"http://{BOARD}/schema"))
        await update.message.reply_text(
            f"Board: {s.get('board')} ({s.get('display')})\n"
            f"Verbs: {', '.join(s.get('verbs', []))}\n"
            f"Guards: {', '.join(s.get('guards', []))}"
        )
    except Exception as e:
        await update.message.reply_text(f"Board unreachable: {e}")


def main():
    if not TOKEN:
        print("ONBOARD_BOT_TOKEN not set — cannot start", file=sys.stderr)
        sys.exit(1)
    if not BOARD:
        print("ONBOARD_BOARD_IP not set — cannot start", file=sys.stderr)
        sys.exit(1)
    app = Application.builder().token(TOKEN).build()
    app.add_handler(CommandHandler("help", cmd_help))
    app.add_handler(CommandHandler("status", cmd_status))
    app.add_handler(CommandHandler("tools", cmd_tools))
    app.add_handler(CommandHandler("load", cmd_load))
    app.add_handler(CommandHandler("schema", cmd_schema))
    print("OneBoard bot polling (board=%s)" % BOARD)
    app.run_polling()


if __name__ == "__main__":
    main()
