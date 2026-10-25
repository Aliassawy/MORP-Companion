#pragma once
#include <Arduino.h>

enum Mood {
  MOOD_NORMAL, MOOD_HAPPY, MOOD_SURPRISED, MOOD_SAD, MOOD_ANGRY,
  MOOD_LOVE, MOOD_SLEEPY, MOOD_SMUG, MOOD_THINKING
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