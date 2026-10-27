# Project MORP 🤖

**MORP** (Modular Open-Source Robotic Personality) is an interactive, AI-powered physical desktop companion. Designed to be more than just a smart speaker, MORP has a physical presence, emotional states, and environmental awareness. 

This project bridges the gap between hardware (ESP32 microcontrollers, sensors, and OLED displays) and an advanced AI backend (Python, FastAPI, LLMs, and WebSockets). MORP is designed as a highly scalable and open platform—while it currently reacts to touch and light, the architecture is built to seamlessly expand into voice interaction, spatial awareness, and gesture recognition.

## 🌟 Vision & Final Aim
The ultimate goal of Project MORP is to create a fully autonomous, emotionally responsive desktop entity. It is not a closed system; it is a modular foundation. The final vision includes:
* **True Voice Interaction:** Moving beyond text terminals to real-time Speech-to-Text (STT) and Text-to-Speech (TTS).
* **Visual Processing:** Upgrading to camera modules for gesture recognition and facial tracking.
* **Persistent Memory:** Evolving AI that remembers past interactions and adapts its personality (currently handled via SQLite).
* **Hardware Modularity:** An open-ended physical chassis that allows developers to plug in new sensors without rewriting the core loop.

## 🛠️ Current Capabilities
* **Dynamic Expressions:** A 0.96" OLED displays various emotions, blinks, and reacts dynamically to events.
* **Physical Interaction:** IR sensors detect when MORP is "poked" or "petted," altering its internal emotional state (happiness, annoyance, etc.).
* **Environmental Awareness:** An LDR (photoresistor) monitors ambient light, allowing MORP to sleep when it's dark to protect the OLED from burn-in.
* **Real-Time Communication:** A robust WebSocket connection links the ESP32 to the Python backend, instantly syncing physical triggers with AI responses.
* **Over-The-Air (OTA) Updates:** Firmware can be flashed over Wi-Fi without plugging the ESP32 into a PC.

## 📦 Hardware Stack
* **Microcontroller:** ESP32 (Wi-Fi enabled)
* **Display:** 0.96" I2C OLED Display (SSD1306, 128x64)
* **Sensors:** 
  * 2x IR Obstacle Avoidance Sensors (Poke/Pet)
  * 1x LDR Photoresistor (Ambient light)

*(See `DOCUMENTATION.md` for information on upcoming audio hardware requirements).*

## 🚀 Quick Start Guide

### 1. The Brain (Python Backend)
1. Clone this repository to your local machine.
2. Create a virtual environment and install dependencies:
   ```bash
   python -m venv venv
   source venv/bin/activate  # Windows: venv\Scripts\activate
   pip install fastapi uvicorn websockets requests
   ```
3. Run the backend server:
   ```bash
   python server.py
   ```
   *The FastAPI server will host the WebSocket at `ws://0.0.0.0:8000/ws` and open the developer terminal interface.*

### 2. The Body (ESP32 Firmware)
1. Open the `.ino` file located in the `/firmware` folder using Arduino IDE or PlatformIO.
2. Update your network settings at the top of the sketch:
   ```cpp
   const char* ssid = "YOUR_WIFI_SSID";
   const char* password = "YOUR_WIFI_PASSWORD";
   const char* websocket_server = "192.168.X.X"; // Your Backend PC's Local IP
   ```
3. Flash the code to your ESP32. 

## 🤝 Contributing
MORP is built to grow. Whether you want to add new OLED animations, design a 3D-printable case, or integrate a new AI model into the Python backend, contributions are welcome! Please read the `DOCUMENTATION.md` file to understand the system architecture before submitting a pull request.