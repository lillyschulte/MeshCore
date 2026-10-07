#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <helpers/IdentityStore.h>   // FILESYSTEM, File
#include <helpers/TxtDataHelpers.h>

#define UI_CANNED_COUNT   8
#define UI_CANNED_LEN    32

#define UI_PREFS_FILE    "/ui_prefs"
#define UI_PREFS_MAGIC   0x4C31   // 'L1'

#define DST_RULE_NONE    0
#define DST_RULE_EU      1   // last Sun of March .. last Sun of October, 01:00 UTC

// UI-only preferences, kept out of NodePrefs so its on-disk format stays upstream compatible
struct UIPrefs {
  uint16_t magic;
  int16_t  tz_offset_min;     // local time = UTC + offset
  uint8_t  clock_24h;
  uint8_t  batt_percent;      // show battery as % instead of an icon (was reserved, old files read 0)
  uint16_t screen_timeout_s;
  char     canned[UI_CANNED_COUNT][UI_CANNED_LEN];
  // --- fields below were added later; older (shorter) files load with them as 0
  uint8_t  dst_rule;          // DST_RULE_*

  void setDefaults() {
    static const char* defaults[UI_CANNED_COUNT] = {
      "OK", "Yes", "No", "On my way", "Call me", "Where are you?", "Help!", "Thanks"
    };
    memset(this, 0, sizeof(*this));
    magic = UI_PREFS_MAGIC;
    tz_offset_min = 0;
    clock_24h = 1;
    screen_timeout_s = 30;
    for (int i = 0; i < UI_CANNED_COUNT; i++) {
      StrHelper::strncpy(canned[i], defaults[i], UI_CANNED_LEN);
    }
  }

  void load(FILESYSTEM* fs) {
    setDefaults();
    if (fs == NULL || !fs->exists(UI_PREFS_FILE)) return;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
    File file = fs->open(UI_PREFS_FILE);
#else
    File file = fs->open(UI_PREFS_FILE, "r");
#endif
    if (!file) return;
    UIPrefs tmp;
    memset(&tmp, 0, sizeof(tmp));
    int n = file.read((uint8_t *)&tmp, sizeof(tmp));
    file.close();
    bool ok = n >= (int) offsetof(UIPrefs, dst_rule);   // accept files from before newer fields were added
    if (ok && tmp.magic == UI_PREFS_MAGIC) {
      *this = tmp;
      for (int i = 0; i < UI_CANNED_COUNT; i++) canned[i][UI_CANNED_LEN - 1] = 0;  // ensure terminated
      if (screen_timeout_s < 5) screen_timeout_s = 30;
    }
  }

  bool save(FILESYSTEM* fs) const {
    if (fs == NULL) return false;
    fs->remove(UI_PREFS_FILE);
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
    File file = fs->open(UI_PREFS_FILE, FILE_O_WRITE);
#else
    File file = fs->open(UI_PREFS_FILE, "w", true);
#endif
    if (!file) return false;
    bool ok = file.write((const uint8_t *)this, sizeof(*this)) == sizeof(*this);
    file.close();
    return ok;
  }
};
