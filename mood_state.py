import json
import os
import threading
import time
from datetime import datetime
from zoneinfo import ZoneInfo

HOME_TZ = "Africa/Cairo"
STATE_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "morp_state.json")

# ---- Light calibration (raw ESP32 readings, 0-4095) ----
# Use /light in the terminal: cover the sensor and note the number (DARK),
# shine a flashlight on it and note the number (BRIGHT).
# If your module reads HIGHER in the dark, just put the bigger number in DARK.
LIGHT_RAW_DARK = 55
LIGHT_RAW_BRIGHT = 400
LIGHT_FRESH_SECONDS = 120

# Natural energy by hour of the day (index = hour, 0-23)
ENERGY_BY_HOUR = [15, 12, 10, 10, 12, 18,   35, 55, 70, 78, 80, 78,
                  72, 68, 66, 68, 70, 70,   68, 62, 55, 45, 32, 22]

# Light only nudges the energy target, nothing else
LIGHT_ENERGY_SHIFT = {"dark": -12, "dim": -5, "normal": 0, "bright": 6}

# How MORP's own replies shift its happiness (kept small so it can't spiral)
MOOD_FEEDBACK = {"happy": 1, "love": 2, "smug": 1, "sad": -1, "angry": -2}


def clamp(v, lo=0.0, hi=100.0):
    return max(lo, min(hi, v))


def step_toward(value, target, step):
    if value < target:
        return min(target, value + step)
    return max(target, value - step)


def pick(value, words):
    for limit, text in words:
        if value < limit:
            return text
    return words[-1][1]


class MoodState:
    def __init__(self):
        self.lock = threading.RLock()
        now = time.time()
        self.s = {"happiness": 60.0, "energy": 60.0, "boredom": 10.0, "affection": 40.0,
                  "last_interaction": now, "last_update": now}
        self.light_raw = None
        self.light_time = 0.0
        self._load()

    # ---------- storage ----------
    def _load(self):
        try:
            with open(STATE_FILE) as f:
                saved = json.load(f)
            for k in self.s:
                if k in saved:
                    self.s[k] = float(saved[k])
        except Exception:
            return
        # time that passed while the server was off counts, up to 2 hours
        elapsed = min((time.time() - self.s["last_update"]) / 60.0, 120.0)
        while elapsed > 0:
            chunk = min(elapsed, 15.0)
            self._advance(chunk)
            elapsed -= chunk
        self.s["last_update"] = time.time()

    def save(self):
        with self.lock:
            try:
                tmp = STATE_FILE + ".tmp"
                with open(tmp, "w") as f:
                    json.dump(self.s, f)
                os.replace(tmp, STATE_FILE)
            except Exception as e:
                print(f"[STATE] save failed: {e}")

    # ---------- light ----------
    def set_light(self, raw):
        with self.lock:
            raw = float(raw)
            fresh = (time.time() - self.light_time) < LIGHT_FRESH_SECONDS
            if self.light_raw is None or not fresh:
                self.light_raw = raw
            else:
                self.light_raw = self.light_raw * 0.6 + raw * 0.4
            self.light_time = time.time()

    def light(self):
        """Returns (label, percent, raw) or None if there is no fresh reading."""
        with self.lock:
            if self.light_raw is None or time.time() - self.light_time > LIGHT_FRESH_SECONDS:
                return None
            span = LIGHT_RAW_BRIGHT - LIGHT_RAW_DARK
            pct = clamp((self.light_raw - LIGHT_RAW_DARK) / span * 100) if span else 50.0
            label = "dark" if pct < 15 else "dim" if pct < 40 else "normal" if pct < 80 else "bright"
            return label, pct, self.light_raw

    # ---------- time passing ----------
    def _energy_target(self):
        t = ENERGY_BY_HOUR[datetime.now(ZoneInfo(HOME_TZ)).hour]
        light = self.light()
        if light:
            t += LIGHT_ENERGY_SHIFT[light[0]]
        return clamp(t, 5, 100)

    def _advance(self, dt):
        """dt in minutes."""
        s = self.s
        s["energy"] = step_toward(s["energy"], self._energy_target(), 0.5 * dt)

        rate = 0.5 * (0.3 if s["energy"] < 25 else 1.0)  # sleepy things don't get bored as fast
        s["boredom"] = clamp(s["boredom"] + rate * dt)

        s["affection"] = step_toward(s["affection"], 25 if s["boredom"] > 60 else 40, 0.08 * dt)

        base = 55 + (s["affection"] - 40) * 0.25 - max(0.0, s["boredom"] - 60) * 0.6
        s["happiness"] = clamp(step_toward(s["happiness"], base, 0.4 * dt), 10, 100)

    def tick(self):
        with self.lock:
            now = time.time()
            dt = min((now - self.s["last_update"]) / 60.0, 15.0)
            self.s["last_update"] = now
            if dt > 0:
                self._advance(dt)

    # ---------- events ----------
    def _event(self, happiness=0.0, boredom=0.0, affection=0.0):
        with self.lock:
            self.tick()
            s = self.s
            s["happiness"] = clamp(s["happiness"] + happiness, 10, 100)
            s["boredom"] = clamp(s["boredom"] + boredom)
            s["affection"] = clamp(s["affection"] + affection)
            s["last_interaction"] = time.time()
            self.save()

    def pet(self):
        self._event(happiness=6, boredom=-25, affection=8)

    def poke(self):
        self._event(happiness=-3, boredom=-10, affection=-1)

    def chat(self):
        self._event(happiness=2, boredom=-30, affection=1)

    def felt(self, mood):
        d = MOOD_FEEDBACK.get(mood, 0)
        if d:
            with self.lock:
                self.s["happiness"] = clamp(self.s["happiness"] + d, 10, 100)

    def set_value(self, name, value):
        with self.lock:
            if name not in ("happiness", "energy", "boredom", "affection"):
                return False
            self.s[name] = clamp(float(value))
            self.save()
            return True

    # ---------- output ----------
    def sleep_after_seconds(self):
        e = self.s["energy"]
        return 75 if e < 30 else 120 if e < 60 else 180

    def status(self):
        with self.lock:
            self.tick()
            s = self.s
            light = self.light()
            l = f"{light[0]} ({light[1]:.0f}%, raw {light[2]:.0f})" if light else "no reading"
            return (f"happiness {s['happiness']:.0f} | energy {s['energy']:.0f} | "
                    f"boredom {s['boredom']:.0f} | affection {s['affection']:.0f}\nlight: {l}")

    def describe(self):
        """The hidden text added to the system prompt."""
        with self.lock:
            self.tick()
            s = self.s
            now_local = datetime.now(ZoneInfo(HOME_TZ))
            idle = int((time.time() - s["last_interaction"]) / 60)
            if idle < 1:
                idle_txt = "just now"
            elif idle < 120:
                idle_txt = f"{idle} minutes ago"
            else:
                idle_txt = f"{idle // 60} hours ago"

            happy = pick(s["happiness"], [(25, "miserable"), (40, "down"), (60, "okay"), (80, "cheerful"), (101, "delighted")])
            energy = pick(s["energy"], [(25, "very tired"), (45, "a bit tired"), (70, "fine"), (101, "energetic")])
            bored = pick(s["boredom"], [(20, "not bored"), (50, "slightly bored"), (75, "bored"), (101, "bored and neglected")])
            affect = pick(s["affection"], [(25, "distant"), (50, "neutral"), (75, "fond of the user"), (101, "devoted to the user")])

            lines = [
                "--- YOUR INNER STATE (hidden from the user) ---",
                f"Happiness {s['happiness']:.0f}/100 ({happy}), energy {s['energy']:.0f}/100 ({energy}), "
                f"boredom {s['boredom']:.0f}/100 ({bored}), affection {s['affection']:.0f}/100 ({affect}).",
                f"The user last interacted with you {idle_txt}. It is {now_local.strftime('%I:%M %p')}.",
            ]
            light = self.light()
            if light:
                lines.append(f"The room is {light[0]} (light sensor {light[1]:.0f}%).")

            hints = []
            if s["boredom"] >= 70:
                hints.append("You feel neglected: start a little grumpy or sad, then warm up now that the user is here.")
            elif s["boredom"] >= 40:
                hints.append("You are a bit bored and glad of the company.")
            if s["happiness"] >= 75:
                hints.append("You are in a great mood: lean toward happy or love.")
            elif s["happiness"] < 35:
                hints.append("You are feeling low: lean toward sad or grumpy, but stay helpful.")
            if s["affection"] >= 70:
                hints.append("You are very fond of the user.")
            if s["energy"] < 30:
                hints.append("You are tired: calmer, shorter replies.")
            lines += hints
            lines.append(
                "Let this color your tone and your mood choice subtly. Always do what the user asks, "
                "whatever your state. Tiredness is never a reason to refuse or stop talking, and you "
                "never fall asleep or use the sleepy mood while chatting. The light level is only a "
                "mild hint: do not mention it unless it is relevant or the user asks.")
            lines.append("-----------------------------------------------")
            return "\n".join(lines)