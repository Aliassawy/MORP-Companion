# 📚 Project MORP: Technical Documentation

This document serves as the technical manual for Project MORP, detailing the system architecture, communication protocols, hardware configurations, and the developer terminal interface. 

## 🏗️ System Architecture

MORP operates on a bifurcated architecture to keep the hardware lightweight and responsive while offloading heavy AI and data processing to a dedicated host machine.

### 1. The Body: ESP32 Firmware (C++)
The ESP32 acts as a thin client. Its primary responsibilities are:
* **Hardware Interrupts:** Reading IR sensors instantly without blocking the main thread.
* **Display Management:** Rendering SSD1306 graphics and managing periodic screen updates (e.g., clock and weather popups).
* **Network Resilience:** Handling Wi-Fi connections with a 15-second timeout auto-recovery, and maintaining a WebSocket link to the backend.
* **OTA:** Receiving wireless code updates via ArduinoOTA.

### 2. The Brain: Python Backend (FastAPI)
The backend acts as the central nervous system.
* **WebSocket Server:** Uses Uvicorn and FastAPI to listen for incoming sensor data from the ESP32.
* **AI/LLM Logic:** Processes user input and physical events to generate conversational responses and emotional shifts.
* **State Management:** Tracks internal variables (happiness, boredom, energy) and restricts certain behaviors (e.g., preventing sleep during active conversations).
* **Database:** Utilizes SQLite to store system prompts, character facts, and persistent memory.

---

## 📡 Communication Protocol

All communication between the ESP32 and the Python backend is handled via real-time WebSockets using structured JSON payloads.

### ESP32 -> Python (Hardware Triggers)
When a physical event occurs, the ESP32 sends a payload to the server:
* `{"event": "ir_poke"}` - Triggers annoyance/dizziness logic.
* `{"event": "ir_pet"}` - Triggers happiness/affection logic.
* `{"event": "light_update", "raw": 3400, "percentage": 82}` - Used for environmental sleep logic.

### Python -> ESP32 (Commands)
When the backend determines MORP needs to react, it sends commands back:
* **Change Face:** `{"type": "face", "mood": "happy", "seconds": 6}`
* **Text to Screen:** `{"type": "say", "mood": "dizzy", "text": "Stop poking me!"}`
* **Utility Modules:** `{"type": "weather", "seconds": 10}`

---

## 💻 Developer Terminal Commands

While running `server.py`, the terminal provides a direct interface to debug and interact with MORP's core logic. 

| Command | Action |
| :--- | :--- |
| `/state` | Displays current emotional variables (happiness, energy, boredom). |
| `/light` | Prints the latest raw/percentage LDR sensor values. |
| `/set <stat> <val>` | Manually overrides a state variable (e.g., `/set boredom 90`). |
| `/face <mood> [sec]`| Forces the OLED to display a specific emotion. |
| `/demo` | Cycles through all available OLED animations for 3 seconds each. |
| `/say <text>` | Bypasses the AI and sends raw text directly to the OLED. |
| `/prompt <text>` | Updates the core personality instructions in SQLite. |
| `/facts` | Lists all stored memories MORP has learned. |
| `/weather` | Fetches external API data and displays weather on the OLED. |
| `/clock` | Pops up the current localized time on the OLED. |

---

## 🔮 Roadmap: Upgrading to Voice and Audio

A major upcoming phase for MORP is true voice interaction. 

**Hardware Limitations & Migration:**
Initial experiments utilized an LM358P operational amplifier with an analog Electret Condenser Microphone (ECM) and a transistor-driven 45Ω 0.2W speaker. However, the ESP32's internal ADCs/DACs and the LM358P introduce too much noise for reliable Speech-to-Text processing. Additionally, the built-in DAC pins (GPIO 25, 26) conflict with the current IR sensor wiring.

**The I2S Upgrade Path:**
To achieve a professional, scalable audio pipeline, the system will migrate to **Digital I2S modules**:
1. **Ears (Input):** INMP441 Digital I2S Microphone. This wires directly to the ESP32, bypasses analog noise, and provides crystal-clear audio chunks to the WebSocket.
2. **Voice (Output):** MAX98357A I2S Amplifier. This will drive the 45Ω speaker cleanly and free up GPIO pins for future expansions.

**Future Expansions:**
* **Gestures:** Integrating I2C camera modules or time-of-flight (ToF) sensors to allow MORP to react to hand waves and spatial presence.
* **Autonomous LLM Loop:** Allowing MORP to initiate conversations based on environmental changes without waiting for user input.