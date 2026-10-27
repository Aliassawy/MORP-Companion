#pragma once
#include <Arduino.h>

// The order must match the name list in display.cpp
enum Mood : uint8_t {
  MOOD_NORMAL, MOOD_HAPPY, MOOD_LOVE, MOOD_SURPRISED, MOOD_ANGRY, MOOD_SAD,
  MOOD_SMUG, MOOD_SLEEPY, MOOD_THINKING, MOOD_EXCITED, MOOD_LAUGH, MOOD_WINK,
  MOOD_SHY, MOOD_CRY, MOOD_SCARED, MOOD_CONFUSED, MOOD_DIZZY, MOOD_DEAD,
  MOOD_BORED, MOOD_SUSPICIOUS, MOOD_STARSTRUCK, MOOD_EVIL, MOOD_SILLY, MOOD_KISS,
  MOOD_WORRIED, MOOD_SICK, MOOD_FOCUSED, MOOD_CHILL, MOOD_ASLEEP, MOOD_YAWN,
  MOOD_DISGUSTED, MOOD_PLEADING, MOOD_COOL, MOOD_GLITCH,
  MOOD_COUNT
};

void displayBegin();
void displayUpdate();
void displayMessage(const char* msg);                        // boot messages
void displayMood(Mood m, unsigned long ms, bool talking = false);
void displaySay(Mood m, const char* text);                   // talks, then shows the text
void displayText(const char* text, unsigned long ms = 0);
void displayWeather(unsigned long ms);
void displayClock(unsigned long ms);
Mood displayParseMood(const char* s);
void displaySetSleepAfter(unsigned long ms);
void displayCue(Mood m, unsigned long ms, bool talk = true);