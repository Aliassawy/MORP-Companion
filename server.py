import asyncio
import json
import logging
import os
import re
import threading
import time
import unicodedata
from collections import deque
from contextlib import asynccontextmanager
from datetime import datetime
from zoneinfo import ZoneInfo

import requests
from fastapi import Body, FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, JSONResponse
from groq import Groq
from database import init_db, get_active_prompt, get_all_facts, save_fact, update_persona
from mood_state import MoodState

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

init_db()
client = Groq(api_key="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")
MODEL = "qwen/qwen3.8-27b"   # must support tool calling; if it errors, try "llama-3.3-70b-versatile"
HOME_CITY = "Cairo"
HOME_TZ = "Africa/Cairo"

state = MoodState()

MOODS = {
    "normal":     "calm, neutral, nothing special (use only when nothing else fits)",
    "happy":      "pleased, friendly, cheerful",
    "love":       "affectionate, adored, loving the attention",
    "surprised":  "startled, caught off guard",
    "angry":      "annoyed, offended, furious",
    "sad":        "hurt, lonely, disappointed",
    "smug":       "sarcastic, teasing, proud",
    "sleepy":     "tired, drowsy (never while chatting)",
    "thinking":   "pondering a hard question",
    "excited":    "thrilled, great news, can't wait",
    "laugh":      "found something really funny",
    "wink":       "playful, joking, sharing a secret",
    "shy":        "embarrassed, flustered, complimented",
    "cry":        "heartbroken, sobbing, very upset",
    "scared":     "frightened, alarmed",
    "confused":   "doesn't understand, puzzled",
    "dizzy":      "overwhelmed, too much at once",
    "dead":       "dying of embarrassment or laughter, comic doom",
    "bored":      "unimpressed, nothing interesting going on",
    "suspicious": "doubtful, side-eyeing a claim",
    "starstruck": "amazed, in awe, dazzled",
    "evil":       "scheming, mischievous plan",
    "silly":      "goofy, playful, tongue out",
    "kiss":       "thankful affection, sending a kiss",
    "worried":    "anxious, concerned for the user",
    "sick":       "unwell, queasy",
    "focused":    "concentrating, determined, working on it",
    "chill":      "relaxed, content, vibing",
    "asleep":     "fast asleep (never while chatting)",
    "yawn":       "yawning",
    "disgusted":  "grossed out, disapproving",
    "pleading":   "begging with puppy eyes",
    "cool":       "smooth, confident, feeling cool",
    "glitch":     "malfunctioning, a system-error joke",
}

ALIASES = {
    "annoyed": "angry", "mad": "angry", "furious": "angry", "joy": "happy", "glad": "happy",
    "amused": "laugh", "embarrassed": "shy", "nervous": "worried", "anxious": "worried",
    "afraid": "scared", "tired": "sleepy", "curious": "thinking", "proud": "smug",
    "sarcastic": "smug", "sorry": "sad", "lonely": "sad", "grateful": "kiss",
    "skeptical": "suspicious", "playful": "wink", "mischievous": "evil", "amazed": "starstruck",
    "sleeping": "asleep", "determined": "focused", "relaxed": "chill",
}
SENSORS = {
    "IR1_Poke": ("poke", "poked your face"),
    "IR2_Pet":  ("pet", "petted you"),
}

SLEEP_WORDS = re.compile(r"\b(night|sleep|bed|tired|nap|rest)\b", re.I)

esp_ws = None
main_loop = None
busy = False
sleep_sent = None
recent = deque(maxlen=5)
history = deque(maxlen=10)
touches = {}
llm_lock = threading.Lock()
last_reply = {"mood": "normal", "text": ""}   # what the website shows as MORP's latest words


# ---------- Sending to the ESP32 ----------
async def send_to_esp(payload: dict):
    if esp_ws is not None:
        try:
            await esp_ws.send_text(json.dumps(payload))
        except Exception:
            pass


def send_from_thread(payload: dict):
    if main_loop is not None and esp_ws is not None:
        asyncio.run_coroutine_threadsafe(send_to_esp(payload), main_loop)


async def push_sleep_after(force: bool = False):
    """Tells the ESP32 how long to stay awake when idle (depends on energy)."""
    global sleep_sent
    secs = state.sleep_after_seconds()
    if force or secs != sleep_sent:
        await send_to_esp({"type": "sleep_after", "seconds": secs})
        sleep_sent = secs


async def state_loop():
    while True:
        await asyncio.sleep(30)
        state.tick()
        state.save()
        await push_sleep_after()


@asynccontextmanager
async def lifespan(app):
    global main_loop
    main_loop = asyncio.get_running_loop()
    logging.getLogger("uvicorn.access").disabled = True   # no HTTP log spam
    task = asyncio.create_task(state_loop())
    yield
    task.cancel()


app = FastAPI(lifespan=lifespan)


def to_ascii(text: str, limit: int = 200) -> str:
    """The OLED font only has plain English characters."""
    for a, b in (("°C", " C"), ("°", ""), ("’", "'"), ("‘", "'"), ("“", '"'),
                 ("”", '"'), ("–", "-"), ("—", "-"), ("…", "...")):
        text = text.replace(a, b)
    text = unicodedata.normalize("NFKD", text).encode("ascii", "ignore").decode()
    text = re.sub(r"[*_`#]", "", text)
    text = re.sub(r"\s+", " ", text).strip()
    return text if len(text) <= limit else text[:limit - 3].rstrip() + "..."


# ---------- Tools the AI can use ----------
def weather_text(code: int) -> str:
    if code == 0: return "clear sky"
    if code in (1, 2): return "partly cloudy"
    if code == 3: return "overcast"
    if code in (45, 48): return "fog"
    if 51 <= code <= 57: return "drizzle"
    if 61 <= code <= 67: return "rain"
    if 71 <= code <= 77: return "snow"
    if 80 <= code <= 82: return "rain showers"
    if code >= 95: return "thunderstorm"
    return "unknown"


def tool_weather(city: str) -> str:
    g = requests.get("https://geocoding-api.open-meteo.com/v1/search",
                     params={"name": city, "count": 1}, timeout=8).json()
    if not g.get("results"):
        return f"Could not find a place called {city}."
    p = g["results"][0]
    w = requests.get("https://api.open-meteo.com/v1/forecast", params={
        "latitude": p["latitude"], "longitude": p["longitude"],
        "current": "temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m",
        "daily": "weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max",
        "timezone": "auto", "forecast_days": 2}, timeout=8).json()
    c, d = w["current"], w["daily"]
    return (
        f"{p['name']}, {p.get('country', '')}: now {c['temperature_2m']}C "
        f"(feels like {c['apparent_temperature']}C), {weather_text(c['weather_code'])}, "
        f"humidity {c['relative_humidity_2m']}%, wind {c['wind_speed_10m']} km/h. "
        f"Today: high {d['temperature_2m_max'][0]}C, low {d['temperature_2m_min'][0]}C, "
        f"rain chance {d['precipitation_probability_max'][0]}%. "
        f"Tomorrow: high {d['temperature_2m_max'][1]}C, low {d['temperature_2m_min'][1]}C, "
        f"rain chance {d['precipitation_probability_max'][1]}%, {weather_text(d['weather_code'][1])}."
    )


def tool_time(tz: str) -> str:
    try:
        now = datetime.now(ZoneInfo(tz))
    except Exception:
        tz = HOME_TZ
        now = datetime.now(ZoneInfo(tz))
    return now.strftime("%A, %d %B %Y, %I:%M %p") + f" ({tz})"


def tool_search(query: str) -> str:
    try:
        from ddgs import DDGS
    except ImportError:
        from duckduckgo_search import DDGS
    results = DDGS().text(query, max_results=4)
    if not results:
        return "No results found."
    return "\n".join(f"- {r.get('title', '')}: {r.get('body', '')}" for r in results)[:1800]


def run_tool(name: str, args: dict) -> str:
    print(f"[TOOL] {name} {args}")
    try:
        if name == "get_weather":
            return tool_weather(args.get("city") or HOME_CITY)
        if name == "get_time":
            return tool_time(args.get("timezone") or HOME_TZ)
        if name == "web_search":
            return tool_search(args.get("query", ""))
        return f"Unknown tool: {name}"
    except Exception as e:
        return f"Tool failed: {e}"


TOOLS = [
    {"type": "function", "function": {
        "name": "get_weather",
        "description": "Get the current weather and today/tomorrow forecast for a city.",
        "parameters": {"type": "object", "properties": {
            "city": {"type": "string", "description": "City name, e.g. Cairo or London"}},
            "required": ["city"]}}},
    {"type": "function", "function": {
        "name": "get_time",
        "description": "Get the current date and time, optionally for another timezone.",
        "parameters": {"type": "object", "properties": {
            "timezone": {"type": "string", "description": "IANA timezone, e.g. Asia/Tokyo. Default Africa/Cairo."}}}}},
    {"type": "function", "function": {
        "name": "web_search",
        "description": "Search the internet for current information: news, facts, prices, scores, how-tos.",
        "parameters": {"type": "object", "properties": {
            "query": {"type": "string", "description": "Short search query"}},
            "required": ["query"]}}},
]


# ---------- Brain ----------
def parse_and_save_facts(response_text: str) -> str:
    pattern = r"\[SAVE_FACT:\s*(.*?)\s*\|\s*(.*?)\]"
    for category, fact_text in re.findall(pattern, response_text):
        save_fact(category, fact_text)
        print(f"\n[MEM SAVED] Category: {category} | Fact: {fact_text}")
    return re.sub(pattern, "", response_text).strip()


def parse_mood(text: str):
    tag = re.search(r"\[MOOD:\s*(\w+)\s*\]", text, re.I)
    mood = tag.group(1).lower() if tag else "normal"
    mood = ALIASES.get(mood, mood)
    if mood not in MOODS:
        mood = "normal"
    return mood, re.sub(r"\[MOOD:\s*\w+\s*\]", "", text, flags=re.I).strip()


def chat(messages: list, use_tools: bool) -> str:
    if not use_tools:
        r = client.chat.completions.create(model=MODEL, messages=messages)
        return r.choices[0].message.content or ""

    for _ in range(4):
        r = client.chat.completions.create(
            model=MODEL, messages=messages, tools=TOOLS, tool_choice="auto")
        msg = r.choices[0].message
        if not msg.tool_calls:
            return msg.content or ""
        messages.append({
            "role": "assistant",
            "content": msg.content or "",
            "tool_calls": [{"id": tc.id, "type": "function",
                            "function": {"name": tc.function.name,
                                         "arguments": tc.function.arguments}}
                           for tc in msg.tool_calls],
        })
        for tc in msg.tool_calls:
            try:
                args = json.loads(tc.function.arguments or "{}")
            except Exception:
                args = {}
            messages.append({"role": "tool", "tool_call_id": tc.id,
                             "content": run_tool(tc.function.name, args)})

    r = client.chat.completions.create(
        model=MODEL, messages=messages, tools=TOOLS, tool_choice="none")
    return r.choices[0].message.content or ""


def query_morp(user_input: str, remember: bool = True, use_tools: bool = True, source: str = "chat"):
    """Returns (mood, reply_text). source is 'chat' or 'touch'."""
    mood_lines = "\n".join(f"- {name}: {desc}" for name, desc in MOODS.items())
    recent_lines = "\n".join(f"- [{m}] {t}" for m, t in recent) or "- (none yet)"

    system = (
        f"{get_active_prompt()}\n\n"
        f"--- USER FACTS DATABASE ---\n{get_all_facts()}\n---------------------------\n\n"
        f"{state.describe()}\n\n"
        f"--- FACE RULES ---\n"
        f"You have a face on a small screen. Begin EVERY reply with one mood tag, "
        f"for example [MOOD: happy], chosen from:\n{mood_lines}\n"
        f"Pick the mood that matches how you genuinely feel about what just happened "
        f"or what you are saying. Use the whole range. Do not default to smug, and do "
        f"not reuse the mood or wording of your recent replies:\n{recent_lines}\n"
        f"------------------\n\n"
        f"--- ASSISTANT RULES ---\n"
        f"You are also the user's personal assistant. The user lives in {HOME_CITY}, Egypt. "
        f"You have tools: get_weather, get_time, web_search. Never guess the weather, "
        f"time, news, prices, scores or any fact that can change: call the right tool "
        f"first. Never say you cannot access the internet. Your answer is shown on a tiny "
        f"128x64 screen, so use plain text only (no markdown, no emojis) and at most "
        f"30 words. Stay in character.\n"
        f"-----------------------"
    )

    with llm_lock:
        messages = [{"role": "system", "content": system}] + list(history) \
                   + [{"role": "user", "content": user_input}]
        raw = chat(messages, use_tools)

    raw = re.sub(r"<think>.*?</think>", "", raw, flags=re.S).strip()
    mood, text = parse_mood(raw)
    text = parse_and_save_facts(text)

    # No falling asleep mid-conversation
    if source == "chat" and mood in ("sleepy", "asleep") and not SLEEP_WORDS.search(user_input):
        mood = "normal"

    state.felt(mood)
    recent.append((mood, text))
    last_reply.update(mood=mood, text=text)   # the website reads this
    if remember:
        history.append({"role": "user", "content": user_input})
        history.append({"role": "assistant", "content": f"[MOOD: {mood}] {text}"})
    return mood, text


# ---------- Website ----------
@app.get("/")
async def serve_site():
    return FileResponse(os.path.join(BASE_DIR, "index.html"))


@app.get("/api/state")
async def api_state():
    state.tick()
    light = state.light()
    s = state.s
    return {
        "happiness": s["happiness"], "energy": s["energy"],
        "boredom": s["boredom"], "affection": s["affection"],
        "light_pct": light[1] if light else None,
        "light_raw": light[2] if light else None,
        "light_label": light[0] if light else None,
        "online": esp_ws is not None,
        "mood": last_reply["mood"], "text": last_reply["text"],
    }


@app.post("/api/face")
async def api_face(d: dict = Body(...)):
    mood = d.get("mood", "normal")
    if mood not in MOODS:
        mood = "normal"
    await send_to_esp({"type": "face", "mood": mood, "seconds": int(d.get("seconds", 6))})
    return {"ok": True}


@app.post("/api/show")
async def api_show(d: dict = Body(...)):
    what = d.get("what")
    if what in ("clock", "weather"):
        await send_to_esp({"type": what, "seconds": 8})
    return {"ok": True}


@app.post("/api/touch")
async def api_touch(d: dict = Body(...)):
    if d.get("kind") == "pet":
        state.pet()
        await send_to_esp({"type": "face", "mood": "love", "seconds": 4})
    else:
        state.poke()
        await send_to_esp({"type": "face", "mood": "surprised", "seconds": 3})
    return {"ok": True}


@app.post("/api/chat")
async def api_chat(d: dict = Body(...)):
    text = (d.get("text") or "").strip()
    if not text:
        return {"mood": "normal", "reply": ""}
    state.chat()
    await send_to_esp({"type": "face", "mood": "thinking", "seconds": 25})
    try:
        mood, reply = await asyncio.to_thread(query_morp, text)
    except Exception as e:
        print(f"[ERROR] {e}")
        await send_to_esp({"type": "face", "mood": "sad", "seconds": 3})
        return JSONResponse({"error": str(e)}, status_code=500)
    await send_to_esp({"type": "say", "mood": mood, "text": to_ascii(reply)})
    return {"mood": mood, "reply": reply}


# ---------- ESP32 connection (only the ESP32 connects here) ----------
@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    global esp_ws, busy
    await websocket.accept()
    esp_ws = websocket
    print("\n[SERVER] ESP32 connected over WebSocket!")
    await push_sleep_after(force=True)

    try:
        while True:
            payload = json.loads(await websocket.receive_text())
            ptype = payload.get("type")

            if ptype == "light":
                try:
                    state.set_light(payload.get("raw", 0))
                except (TypeError, ValueError):
                    pass
                continue

            if ptype != "sensor_trigger":
                continue

            sensor = payload.get("sensor")
            kind, verb = SENSORS.get(sensor, ("touch", "touched you"))
            print(f"\n[PHYSICAL EVENT] Triggered: {sensor}")

            if kind == "pet":
                state.pet()
            elif kind == "poke":
                state.poke()

            now = time.time()
            log = touches.setdefault(kind, deque(maxlen=10))
            log.append(now)
            count = sum(1 for t in log if now - t < 20)

            if busy:
                continue
            busy = True
            try:
                event = (
                    f"*System: The user just {verb} (touch number {count} of this kind "
                    f"in the last 20 seconds).* A poke is a quick jab at your face, a pet "
                    f"is a gentle stroke. React in one short sentence. Your reaction should "
                    f"change with how often it happens: surprise the first time, stronger "
                    f"feelings if it keeps repeating."
                )
                mood, reply = await asyncio.to_thread(query_morp, event, False, False, "touch")
                print(f"MORP [{mood}]: {reply}")
                await send_to_esp({"type": "say", "mood": mood, "text": to_ascii(reply)})
            except Exception as e:
                print(f"[ERROR] {e}")
            finally:
                busy = False

    except WebSocketDisconnect:
        print("\n[SERVER] ESP32 disconnected.")
    finally:
        if esp_ws is websocket:
            esp_ws = None


# ---------- Terminal ----------
def terminal_chat_loop():
    print("\n================ MORP TERMINAL ================")
    print("• Website: http://localhost:8000")
    print("• Type to chat (it can check weather, time and search the web).")
    print("• '/state'                  show MORP's current feelings + light")
    print("• '/light'                  show the light sensor (for calibration)")
    print("• '/set <stat> <0-100>'     e.g. /set boredom 90 (happiness, energy, boredom, affection)")
    print("• '/face <mood> [seconds]'  pick a face, e.g. /face love 8")
    print("• '/moods'  '/clock'  '/weather'  '/prompt <text>'  '/facts'")
    print("===============================================")

    while True:
        user_msg = input("\nYou: ").strip()
        if not user_msg:
            continue

        if user_msg.startswith("/prompt "):
            update_persona(user_msg[len("/prompt "):].strip())
            print("[SYSTEM] Character prompt updated in SQLite database!")

        elif user_msg == "/facts":
            print("\n--- STORED FACTS ---")
            print(get_all_facts())

        elif user_msg == "/state":
            print(state.status())

        elif user_msg == "/light":
            light = state.light()
            if light:
                print(f"Light: raw {light[2]:.0f} -> {light[1]:.0f}% ({light[0]})")
            else:
                print("No light reading yet (is the ESP32 connected and wired to GPIO 32?)")

        elif user_msg.startswith("/set"):
            parts = user_msg.split()
            if len(parts) == 3 and parts[2].replace(".", "", 1).isdigit() and state.set_value(parts[1], parts[2]):
                print(state.status())
            else:
                print("Usage: /set <happiness|energy|boredom|affection> <0-100>")

        elif user_msg == "/moods":
            print("Moods: " + ", ".join(MOODS))

        elif user_msg == "/clock":
            send_from_thread({"type": "clock", "seconds": 8})

        elif user_msg == "/weather":
            send_from_thread({"type": "weather"})

        elif user_msg.startswith("/face"):
            parts = user_msg.split()
            mood = parts[1].lower() if len(parts) > 1 else ""
            if mood not in MOODS:
                print("Pick one of: " + ", ".join(MOODS))
                continue
            secs = int(parts[2]) if len(parts) > 2 and parts[2].isdigit() else 6
            send_from_thread({"type": "face", "mood": mood, "seconds": secs})

        elif user_msg.startswith("/say "):      # test the screen without the AI
            text = to_ascii(user_msg[5:])
            print("Sending to screen:", text)
            send_from_thread({"type": "say", "mood": "happy", "text": text})

        elif user_msg == "/demo":               # shows every face for 3 seconds
            for m in MOODS:
                print(m)
                send_from_thread({"type": "face", "mood": m, "seconds": 3})
                time.sleep(3.2)

        else:
            state.chat()
            send_from_thread({"type": "face", "mood": "thinking", "seconds": 25})
            try:
                mood, reply = query_morp(user_msg)
            except Exception as e:
                print(f"[ERROR] {e}")
                send_from_thread({"type": "face", "mood": "sad", "seconds": 3})
                continue
            print(f"MORP [{mood}]: {reply}")
            send_from_thread({"type": "say", "mood": mood, "text": to_ascii(reply)})


if __name__ == "__main__":
    import copy
    import uvicorn
    from uvicorn.config import LOGGING_CONFIG

    # Silence uvicorn's per-request access lines so they don't bury your chat
    log_config = copy.deepcopy(LOGGING_CONFIG)
    log_config["loggers"]["uvicorn.access"]["handlers"] = []
    log_config["loggers"]["uvicorn.access"]["propagate"] = False

    threading.Thread(target=terminal_chat_loop, daemon=True).start()
    uvicorn.run(app, host="0.0.0.0", port=8000, log_config=log_config)