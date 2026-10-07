#include "UITask.h"
#include <helpers/TxtDataHelpers.h>
#include "../MyMesh.h"
#include "target.h"
#if defined(NRF52_PLATFORM)
  #include <InternalFileSystem.h>
#endif

#include "Widgets.h"
#include "Keyboard.h"
#include "Screens.h"

#if defined(__has_include)
  #if __has_include("splash_custom.h")
    #include "splash_custom.h"   // local, git-ignored splash customisation (see splash_custom.h.example)
  #endif
#endif


#ifdef PIN_STATUS_LED
#define LED_ON_MILLIS     20
#define LED_ON_MSG_MILLIS 200
#define LED_CYCLE_MILLIS  4000
#endif

#define JOY_DEBOUNCE_MILLIS     15
#define JOY_REPEAT_DELAY_MILLIS 450
#define JOY_REPEAT_MILLIS       120

#ifndef UI_RECENT_LIST_SIZE
  #define UI_RECENT_LIST_SIZE 4
#endif

// 2025-01-01. Without an RTC chip the clock boots at 15 May 2024 (VolatileRTCClock), so anything
// before this means the time was never synced
#define MIN_VALID_EPOCH  1735689600UL

#include "icons.h"

static const char* const weekdays[] = { "Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed" };   // 1970-01-01 was a Thursday
static const char* const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// days since 1970-01-01 -> civil date (Howard Hinnant's algorithm)
// civil date -> days since 1970-01-01 (Howard Hinnant's algorithm)
static int32_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  int32_t era = (y >= 0 ? y : y - 399) / 400;
  uint32_t yoe = (uint32_t)(y - era * 400);
  uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  uint32_t doe = yoe * 365 + yoe/4 - yoe/100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

// days since epoch of the last Sunday in month m (m = 3 or 10, both have 31 days)
static int32_t lastSundayOf(int y, int m) {
  int32_t last = daysFromCivil(y, m, 31);
  int weekday = (last + 4) % 7;   // 0 = Sunday (1970-01-01 was a Thursday)
  return last - weekday;
}

static void civilFromDays(int32_t z, int& y, int& m, int& d) {
  z += 719468;
  int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097);
  uint32_t yoe = (doe - doe/1460 + doe/36524 - doe/146096) / 365;
  int32_t yy = (int32_t)yoe + era * 400;
  uint32_t doy = doe - (365*yoe + yoe/4 - yoe/100);
  uint32_t mp = (5*doy + 2) / 153;
  d = doy - (153*mp + 2)/5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = yy + (m <= 2);
}

// EU summer time: from the last Sunday of March to the last Sunday of October, switching at 01:00 UTC
static bool isEUSummerTime(uint32_t utc) {
  int y, m, d;
  civilFromDays(utc / 86400, y, m, d);
  uint32_t start = (uint32_t) lastSundayOf(y, 3) * 86400 + 3600;
  uint32_t end = (uint32_t) lastSundayOf(y, 10) * 86400 + 3600;
  return utc >= start && utc < end;
}

class SplashScreen : public UIScreen {
  UITask* _task;
  unsigned long dismiss_after;
  char _version_info[12];

public:
  SplashScreen(UITask* task, unsigned long duration_millis) : _task(task) {
    // strip off dash and commit hash by changing dash to null terminator
    // e.g: v1.2.3-abcdef -> v1.2.3
    const char *ver = FIRMWARE_VERSION;
    const char *dash = strchr(ver, '-');

    int len = dash ? dash - ver : strlen(ver);
    if (len >= sizeof(_version_info)) len = sizeof(_version_info) - 1;
    memcpy(_version_info, ver, len);
    _version_info[len] = 0;

    dismiss_after = millis() + duration_millis;
  }

  int render(DisplayDriver& display) override {
    // meshcore logo
    display.setColor(UIColor::corp_blue);
    int logoWidth = 128;
    display.drawXbm((display.width() - logoWidth) / 2, 3, meshcore_logo, logoWidth, 13);

#ifdef SPLASH_SUBTITLE
    // custom line under the logo, the rest moves down to make room
    display.setColor(UIColor::primary_txt);
    display.setTextSize(1);
    display.drawTextCentered(display.width()/2, 19, SPLASH_SUBTITLE);
    const int website_y = 30, version_y = 41, date_y = 52;
#else
    const int website_y = 22, version_y = 35, date_y = 48;
#endif

    // meshcore website
    const char* website = "https://meshcore.io";
    display.setColor(UIColor::primary_txt);
    display.setTextSize(1);
    uint16_t websiteWidth = display.getTextWidth(website);
    display.setCursor((display.width() - websiteWidth) / 2, website_y);
    display.print(website);

    // version info
    display.setColor(UIColor::primary_txt);
    display.setTextSize(1);
    display.drawTextCentered(display.width()/2, version_y, _version_info);

    display.setColor(UIColor::secondary_txt);
    display.setTextSize(1);
    display.drawTextCentered(display.width()/2, date_y, FIRMWARE_BUILD_DATE);

    return 1000;
  }

  void poll() override {
    if (millis() >= dismiss_after) {
      _task->gotoHomeScreen();
    }
  }
};

class HomeScreen : public UIScreen {
  enum HomePage {
    CLOCK,
    MESSAGES,
    RECENT,
    RADIO,
#if ENV_INCLUDE_GPS == 1
    GPS,
#endif
    ADVERT,
    Count    // keep as last
  };

  UITask* _task;
  mesh::RTCClock* _rtc;
  NodePrefs* _node_prefs;
  uint8_t _page;
  int _msg_sel;   // messages page: 0 = newest received message
  bool _advert_flood;   // advert page: up/down switches zero-hop <-> flood
  AdvertPath recent[UI_RECENT_LIST_SIZE];

  // n-th newest received (incoming) message, and the total count
  UIMsg* getReceived(int n, int& total) {
    MsgStore& store = _task->msgs();
    UIMsg* found = NULL;
    total = 0;
    for (int i = store.count() - 1; i >= 0; i--) {
      UIMsg* m = store.get(i);
      if (m->flags & MSG_FLAG_OUT) continue;
      if (total == n) found = m;
      total++;
    }
    return found;
  }

  void renderMessagesPage(DisplayDriver& display) {
    int total;
    if (_msg_sel < 0) _msg_sel = 0;
    UIMsg* m = getReceived(_msg_sel, total);
    if (m == NULL && total > 0) {
      _msg_sel = total - 1;
      m = getReceived(_msg_sel, total);
    }
    display.setTextSize(1);
    display.setColor(UIColor::primary_txt);
    if (m == NULL) {
      display.drawTextCentered(display.width() / 2, 28, "No messages yet");
      return;
    }
    m->flags &= ~MSG_FLAG_UNREAD;   // it's on screen now

    // header: conversation name, position and age
    char tmp[24], age[8] = "";
    if (_task->isClockSet()) formatAge(age, _rtc->getCurrentTime() - m->timestamp);
    snprintf(tmp, sizeof(tmp), "%d/%d %s", _msg_sel + 1, total, age);
    int right_w = display.getTextWidth(tmp);
    display.setCursor(display.width() - right_w, 18);
    display.print(tmp);

    char name[32], filtered[48];
    _task->getChatName(m->key, name, sizeof(name));
    display.translateUTF8ToBlocks(filtered, name, sizeof(filtered));
    display.drawTextEllipsized(0, 18, display.width() - right_w - 4, filtered);
    display.fillRect(0, 27, display.width(), 1);

    // message body, wrapped; "..." when it does not fit (Enter -> Open chat shows all)
    char body[UI_MSG_TEXT_LEN + 1];
    display.translateUTF8ToBlocks(body, m->text, sizeof(body));
    const int max_lines = 4;
    int line = 0;
    int total_lines = wrapText(body, 21, [&](const char* s, int len) {
      if (line < max_lines) {
        char seg[22];
        memcpy(seg, s, len);
        seg[len] = 0;
        display.setCursor(0, 29 + line * 9);
        display.print(seg);
      }
      line++;
    });
    if (total_lines > max_lines) {
      display.setColor(UIColor::window_bkg);
      display.fillRect(display.width() - 18, 29 + (max_lines - 1) * 9, 18, 8);
      display.setColor(UIColor::primary_txt);
      display.setCursor(display.width() - 18, 29 + (max_lines - 1) * 9);
      display.print("...");
    }
  }

  void renderBatteryIndicator(DisplayDriver& display, uint16_t batteryMilliVolts) {
#ifndef BATT_MIN_MILLIVOLTS
  #define BATT_MIN_MILLIVOLTS 3000
#endif
#ifndef BATT_MAX_MILLIVOLTS
  #define BATT_MAX_MILLIVOLTS 4200
#endif
    const int minMilliVolts = BATT_MIN_MILLIVOLTS;
    const int maxMilliVolts = BATT_MAX_MILLIVOLTS;
    int batteryPercentage = ((batteryMilliVolts - minMilliVolts) * 100) / (maxMilliVolts - minMilliVolts);
    if (batteryPercentage < 0) batteryPercentage = 0;
    if (batteryPercentage > 100) batteryPercentage = 100;

    int iconWidth = 24;
    int iconHeight = 10;
    int iconX = display.width() - iconWidth - 5;
    int iconY = 0;
    display.setColor(UIColor::title_txt);
    if (_task->uiPrefs().batt_percent) {
      char pct[8];
      sprintf(pct, "%d%%", batteryPercentage);
      display.setTextSize(1);
      display.drawTextRightAlign(display.width() - 1, 2, pct);
    } else {
      display.drawRect(iconX, iconY, iconWidth, iconHeight);
      display.fillRect(iconX + iconWidth, iconY + (iconHeight / 4), 3, iconHeight / 2);
      int fillWidth = (batteryPercentage * (iconWidth - 4)) / 100;
      display.fillRect(iconX + 2, iconY + 2, fillWidth, iconHeight - 4);
    }

#ifdef PIN_BUZZER
    if (_task->isBuzzerQuiet()) {
      display.setColor(UIColor::warning_txt);
      display.drawXbm(iconX - 9, iconY + 1, muted_icon, 8, 8);
    }
#endif
  }

  void renderClockPage(DisplayDriver& display) {
    char tmp[32];
    if (_task->isClockSet()) {
      uint32_t t = _task->getLocalTime();
      _task->formatClock(t, tmp, false);
      display.setTextSize(3);
      display.setColor(UIColor::primary_txt);
      int w = display.getTextWidth(tmp);
      bool pm_label = !_task->uiPrefs().clock_24h;
      int x = (display.width() - w - (pm_label ? 14 : 0)) / 2;
      display.setCursor(x, 18);
      display.print(tmp);
      if (pm_label) {
        display.setTextSize(1);
        display.setCursor(x + w + 2, 33);
        display.print(((t / 3600) % 24) < 12 ? "AM" : "PM");
      }

      int y, m, d;
      int32_t days = t / 86400;
      civilFromDays(days, y, m, d);
      snprintf(tmp, sizeof(tmp), "%s %d %s %d", weekdays[days % 7], d, months[m - 1], y);
      display.setTextSize(1);
      display.drawTextCentered(display.width() / 2, 44, tmp);
    } else {
      display.setTextSize(3);
      display.setColor(UIColor::primary_txt);
      display.drawTextCentered(display.width() / 2, 18, "--:--");
      display.setTextSize(1);
      display.drawTextCentered(display.width() / 2, 44, "sync time via app");
    }

    // status line
    display.setTextSize(1);
    int unread = _task->msgs().unreadCount();
    if (unread > 0) {
      snprintf(tmp, sizeof(tmp), "%d new message%s", unread, unread == 1 ? "" : "s");
      display.setColor(UIColor::primary_txt);
      display.fillRect(0, 54, display.width(), 10);
      display.setColor(UIColor::window_bkg);
      display.drawTextCentered(display.width() / 2, 55, tmp);
      display.setColor(UIColor::primary_txt);
    } else if (_task->hasConnection()) {
      display.drawTextCentered(display.width() / 2, 55, "< Connected >");
    } else if (_task->isBluetoothEnabled() && the_mesh.getBLEPin() != 0) {
      snprintf(tmp, sizeof(tmp), "BLE pin: %d", (int) the_mesh.getBLEPin());
      display.drawTextCentered(display.width() / 2, 55, tmp);
    }
  }

public:
  HomeScreen(UITask* task, mesh::RTCClock* rtc, NodePrefs* node_prefs)
     : _task(task), _rtc(rtc), _node_prefs(node_prefs), _page(0), _msg_sel(0), _advert_flood(false) { }

  void showNewestMessage() {
    _page = HomePage::MESSAGES;
    _msg_sel = 0;
  }

  int render(DisplayDriver& display) override {
    char tmp[80];
    // node name
    display.setTextSize(1);
    display.setColor(UIColor::title_txt);
    char filtered_name[sizeof(_node_prefs->node_name)];
    display.translateUTF8ToBlocks(filtered_name, _node_prefs->node_name, sizeof(filtered_name));
    display.drawTextEllipsized(0, 2, display.width() - 42, filtered_name);

    renderBatteryIndicator(display, _task->getBattMilliVolts());

    // curr page indicator
    display.setColor(UIColor::title_txt);
    int y = 13;
    int x = display.width() / 2 - 5 * (HomePage::Count-1);
    for (uint8_t i = 0; i < HomePage::Count; i++, x += 10) {
      if (i == _page) {
        display.fillRect(x-1, y-1, 4, 4);
      } else {
        display.fillRect(x, y, 2, 2);
      }
    }

    if (_page == HomePage::CLOCK) {
      renderClockPage(display);
      return 1000;   // keep the clock (and its colon) current
    } else if (_page == HomePage::MESSAGES) {
      renderMessagesPage(display);
      return 5000;
    } else if (_page == HomePage::ADVERT) {
      display.setColor(UIColor::corp_blue);
      display.drawXbm((display.width() - 32) / 2, 18, advert_icon, 32, 32);
      display.setColor(UIColor::secondary_txt);
      display.setTextSize(1);
      display.drawTextLeftAlign(90, 24, "\x18\x19");   // up/down arrows: switch mode
      display.drawTextLeftAlign(90, 34, _advert_flood ? "flood" : "0-hop");
      display.drawTextCentered(display.width() / 2, 64 - 11,
                               _advert_flood ? "flood: press Enter" : "0-hop: press Enter");
    } else if (_page == HomePage::RECENT) {
      the_mesh.getRecentlyHeard(recent, UI_RECENT_LIST_SIZE - 1);
      display.setColor(UIColor::primary_txt);
      display.drawTextLeftAlign(0, 18, "Recently heard:");
      if (recent[0].name[0] == 0) {
        display.drawTextCentered(display.width() / 2, 36, "nothing yet");
      }
      int y = 29;
      for (int i = 0; i < UI_RECENT_LIST_SIZE - 1; i++, y += 11) {
        auto a = &recent[i];
        if (a->name[0] == 0) continue;  // empty slot
        formatAge(tmp, _rtc->getCurrentTime() - a->recv_timestamp);
        int timestamp_width = display.getTextWidth(tmp);
        int max_name_width = display.width() - timestamp_width - 1;

        char filtered_recent_name[sizeof(a->name)];
        display.translateUTF8ToBlocks(filtered_recent_name, a->name, sizeof(filtered_recent_name));
        display.drawTextEllipsized(0, y, max_name_width, filtered_recent_name);
        display.setCursor(display.width() - timestamp_width - 1, y);
        display.print(tmp);
      }
    } else if (_page == HomePage::RADIO) {
      display.setColor(UIColor::primary_txt);
      display.setTextSize(1);
      display.setCursor(0, 20);
      sprintf(tmp, "FQ: %06.3f   SF: %d", _node_prefs->freq, _node_prefs->sf);
      display.print(tmp);

      display.setCursor(0, 31);
      sprintf(tmp, "BW: %03.2f     CR: %d", _node_prefs->bw, _node_prefs->cr);
      display.print(tmp);

      display.setCursor(0, 42);
      sprintf(tmp, "TX: %ddBm", _node_prefs->tx_power_dbm);
      display.print(tmp);
      display.setCursor(0, 53);
      sprintf(tmp, "Noise floor: %d", radio_driver.getNoiseFloor());
      display.print(tmp);
#if ENV_INCLUDE_GPS == 1
    } else if (_page == HomePage::GPS) {
      LocationProvider* nmea = sensors.getLocationProvider();
      char buf[50];
      int y = 18;
      bool gps_state = _task->getGPSState();
      strcpy(buf, gps_state ? "gps on" : "gps off");
      display.setColor(UIColor::primary_txt);
      display.drawTextLeftAlign(0, y, buf);
      if (nmea == NULL) {
        y = y + 12;
        display.drawTextLeftAlign(0, y, "Can't access GPS");
      } else {
        strcpy(buf, nmea->isValid()?"fix":"no fix");
        display.drawTextRightAlign(display.width()-1, y, buf);
        y = y + 12;
        display.drawTextLeftAlign(0, y, "sat");
        sprintf(buf, "%d", nmea->satellitesCount());
        display.drawTextRightAlign(display.width()-1, y, buf);
        y = y + 12;
        display.drawTextLeftAlign(0, y, "pos");
        sprintf(buf, "%.4f %.4f",
          nmea->getLatitude()/1000000., nmea->getLongitude()/1000000.);
        display.drawTextRightAlign(display.width()-1, y, buf);
        y = y + 12;
        display.drawTextLeftAlign(0, y, "alt");
        sprintf(buf, "%.2f", nmea->getAltitude()/1000.);
        display.drawTextRightAlign(display.width()-1, y, buf);
      }
#endif
    }
    return 5000;
  }

  bool handleInput(char c) override {
    if (c == KEY_LEFT || c == KEY_PREV) {
      _page = (_page + HomePage::Count - 1) % HomePage::Count;
      return true;
    }
    if (c == KEY_RIGHT || c == KEY_NEXT) {
      _page = (_page + 1) % HomePage::Count;
      return true;
    }
    if (_page == HomePage::MESSAGES) {
      int total;
      UIMsg* m = getReceived(_msg_sel, total);
      if (c == KEY_DOWN) {   // older
        if (_msg_sel < total - 1) _msg_sel++;
        return true;
      }
      if (c == KEY_UP) {     // newer
        if (_msg_sel > 0) _msg_sel--;
        return true;
      }
      if (c == KEY_ENTER && m) {
        _task->openReply(m->key, true);
        return true;
      }
    }
    if (_page == HomePage::ADVERT) {
      if (c == KEY_UP || c == KEY_DOWN) {
        _advert_flood = !_advert_flood;
        return true;
      }
      if (c == KEY_ENTER) {
        _task->notify(UIEventType::ack);
        bool ok = the_mesh.advert(_advert_flood);
        _task->showAlert(!ok ? "Advert failed.." : (_advert_flood ? "Flood advert sent!" : "Advert sent!"), 1000);
        return true;
      }
    }
    if (c == KEY_ENTER) {
      _task->openMenu();
      return true;
    }
    return false;
  }
};

/* ------------------------------------------------------------------ UITask */

void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs) {
  _display = display;
  _sensors = sensors;

#if defined(NRF52_PLATFORM)
  _ui_prefs.load(&InternalFS);
#else
  _ui_prefs.setDefaults();
#endif
  _auto_off = millis() + autoOffMillis();

#if defined(PIN_USER_BTN)
  user_btn.begin();
#endif
#if UI_HAS_JOYSTICK
  joystick_left.begin();
  joystick_right.begin();
  joystick_up.begin();
  joystick_down.begin();
  back_btn.begin();
  MomentaryButton* dirs[4] = { &joystick_up, &joystick_down, &joystick_left, &joystick_right };
  const char keys[4] = { KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT };
  for (int i = 0; i < 4; i++) {
    _joy[i].btn = dirs[i];
    _joy[i].key = keys[i];
    _joy[i].raw = _joy[i].down = false;
    _joy[i].raw_at = _joy[i].next_repeat = 0;
  }
#endif

  _node_prefs = node_prefs;

  if (_display != NULL) {
    _display->turnOn();
  }

#ifdef PIN_BUZZER
  buzzer.begin();
  buzzer.quiet(_node_prefs->buzzer_quiet);
  buzzer.startup();
#endif

#ifdef PIN_VIBRATION
  vibration.begin();
#endif

  ui_started_at = millis();
  _alert_expiry = 0;

  splash = new SplashScreen(this, (unsigned long)_ui_prefs.splash_secs * 1000UL);
  home = new HomeScreen(this, &rtc_clock, node_prefs);
  menu = new MainMenuScreen(this);
  threads = new ThreadsScreen(this);
  contacts = new ContactsScreen(this);
  channels = new ChannelsScreen(this);
  chat = new ChatScreen(this);
  reply = new ReplyScreen(this);
  settings = new SettingsScreen(this);
  canned_edit = new CannedEditScreen(this);
  keyboard = new KeyboardScreen(this);

  _stack_len = 0;
  if (_ui_prefs.splash_secs > 0) {
    push(splash);
  } else {
    gotoHomeScreen();   // splash turned off in Settings
  }
}

/* navigation */

void UITask::setCurrScreen(UIScreen* c) {
  bool was_in_settings = stackContains(settings);
  _stack[0] = c;
  _stack_len = 1;
  _next_refresh = 100;
  flushSettingsIfLeft(was_in_settings);
}

bool UITask::stackContains(UIScreen* s) const {
  for (int i = 0; i < _stack_len; i++) {
    if (_stack[i] == s) return true;
  }
  return false;
}

void UITask::flushSettingsIfLeft(bool was_in_settings) {
  if (was_in_settings && !stackContains(settings)) flushSettings(true);
}

// write settings changed while in the Settings screens, in one go
void UITask::flushSettings(bool show_popup) {
  if (!_ui_prefs_dirty && !_node_prefs_dirty) return;

  show_popup = show_popup && _display != NULL && _display->isOn();
  if (show_popup) {
    showAlert("Saving...", 2000);
    renderFrame();   // show it now, the writes below block
  }
  if (_ui_prefs_dirty) saveUIPrefs();
  if (_node_prefs_dirty) the_mesh.savePrefs();
  _ui_prefs_dirty = _node_prefs_dirty = false;
  if (show_popup) showAlert("Saved", 600);
  _next_refresh = 0;
}

void UITask::gotoHomeScreen() { setCurrScreen(home); }

void UITask::push(UIScreen* s) {
  if (_stack_len > 0 && _stack[_stack_len - 1] == s) return;   // already showing
  if (_stack_len >= UI_NAV_STACK_SIZE) {   // drop the oldest (but keep home at the bottom)
    memmove(&_stack[1], &_stack[2], sizeof(_stack[0]) * (UI_NAV_STACK_SIZE - 2));
    _stack_len--;
  }
  _stack[_stack_len++] = s;
  _next_refresh = 0;
}

void UITask::pop() {
  if (_stack_len > 1) {
    bool was_in_settings = stackContains(settings);
    _stack_len--;
    flushSettingsIfLeft(was_in_settings);
  } else {
    setCurrScreen(home);
  }
  _next_refresh = 0;
}

void UITask::openMenu() {
  ((MainMenuScreen *) menu)->reset();
  push(menu);
}
void UITask::openThreads() {
  ((ThreadsScreen *) threads)->reset();
  push(threads);
}
void UITask::openContacts() {
  ((ContactsScreen *) contacts)->refresh();
  push(contacts);
}
void UITask::openChannels() {
  ((ChannelsScreen *) channels)->refresh();
  push(channels);
}
void UITask::openSettings() {
  ((SettingsScreen *) settings)->reset();
  push(settings);
}
void UITask::openCannedEdit() {
  ((CannedEditScreen *) canned_edit)->open();
  push(canned_edit);
}
void UITask::openChat(const ChatKey& key) {
  // only one chat on the stack at a time
  for (int i = 0; i < _stack_len; i++) {
    if (_stack[i] == chat || _stack[i] == reply || _stack[i] == keyboard) {
      _stack_len = i;
      break;
    }
  }
  ((ChatScreen *) chat)->open(key);
  push(chat);
}
void UITask::openReply(const ChatKey& key, bool offer_open_chat) {
  ((ReplyScreen *) reply)->open(key, offer_open_chat);
  push(reply);
}
void UITask::openKeyboard(const char* title, const char* initial, int max_len, KeyboardListener* listener, int tag) {
  ((KeyboardScreen *) keyboard)->start(title, initial, max_len, listener, tag);
  push(keyboard);
}

/* data helpers */

void UITask::saveUIPrefs() {
#if defined(NRF52_PLATFORM)
  _ui_prefs.save(&InternalFS);
#endif
}

void UITask::getChatName(const ChatKey& key, char* dest, int dest_size) {
  if (key.isChannel()) {
    ChannelDetails ch;
    if (the_mesh.getChannel(key.channelIdx(), ch) && ch.name[0]) {
      StrHelper::strncpy(dest, ch.name, dest_size);
    } else {
      snprintf(dest, dest_size, "Channel %d", key.channelIdx());
    }
  } else {
    ContactInfo* c = the_mesh.lookupContactByPubKey(key.id, sizeof(key.id));
    if (c) {
      StrHelper::strncpy(dest, c->name, dest_size);
    } else {
      snprintf(dest, dest_size, "<%02x%02x%02x>", key.id[0], key.id[1], key.id[2]);
    }
  }
}

int UITask::getMaxComposeLen(const ChatKey& key) const {
  if (key.isChannel()) {   // channel msgs are sent as "name: text"
    int n = MAX_TEXT_LEN - strlen(_node_prefs->node_name) - 2;
    return n < UI_MAX_COMPOSE_LEN ? n : UI_MAX_COMPOSE_LEN;
  }
  return UI_MAX_COMPOSE_LEN;
}

bool UITask::sendText(const ChatKey& key, const char* text) {
  uint32_t now = rtc_clock.getCurrentTime();
  if (key.isChannel()) {
    bool ok = the_mesh.uiSendChannel(key.channelIdx(), text);
    _msgs.add(key, MSG_FLAG_OUT | (ok ? 0 : MSG_FLAG_FAILED), now, "", text);
    showAlert(ok ? "Sent" : "Send failed", 800);
    _next_refresh = 0;
    return ok;
  }

  ContactInfo* recipient = the_mesh.lookupContactByPubKey(key.id, sizeof(key.id));
  if (recipient == NULL) {
    showAlert("Unknown contact", 1000);
    return false;
  }
  uint32_t expected_ack = 0, est_timeout = 0;
  int result = the_mesh.uiSendDirect(*recipient, text, expected_ack, est_timeout);
  UIMsg* m = _msgs.add(key, MSG_FLAG_OUT, now, "", text);
  if (result == MSG_SEND_FAILED) {
    m->flags |= MSG_FLAG_FAILED;
    showAlert("Send failed", 1000);
  } else {
    m->ack = expected_ack;
    if (expected_ack) {
      m->flags |= MSG_FLAG_PENDING;
      m->deadline = millis() + est_timeout + 5000;
    }
    showAlert(result == MSG_SEND_SENT_FLOOD ? "Sent (flood)" : "Sent (direct)", 800);
  }
  _next_refresh = 0;
  return result != MSG_SEND_FAILED;
}

bool UITask::isClockSet() const {
  return rtc_clock.getCurrentTime() >= MIN_VALID_EPOCH;
}

uint32_t UITask::getLocalTime() const {
  uint32_t utc = rtc_clock.getCurrentTime();
  uint32_t t = utc + (int32_t)_ui_prefs.tz_offset_min * 60;
  if (_ui_prefs.dst_rule == DST_RULE_EU && isEUSummerTime(utc)) t += 3600;
  return t;
}

void UITask::formatClock(uint32_t local_time, char* dest, bool with_ampm) const {
  int hours = (local_time / 3600) % 24;
  int mins = (local_time / 60) % 60;
  if (_ui_prefs.clock_24h) {
    sprintf(dest, "%02d:%02d", hours, mins);
  } else {
    int h12 = hours % 12;
    if (h12 == 0) h12 = 12;
    if (with_ampm) {
      sprintf(dest, "%d:%02d%s", h12, mins, hours < 12 ? "am" : "pm");
    } else {
      sprintf(dest, "%d:%02d", h12, mins);
    }
  }
}

/* mesh events */

void UITask::showAlert(const char* text, int duration_millis) {
  StrHelper::strncpy(_alert, text, sizeof(_alert));
  _alert_expiry = millis() + duration_millis;
}

void UITask::notify(UIEventType t) {
#if defined(PIN_BUZZER)
switch(t){
  case UIEventType::contactMessage:
    // gemini's pick
    buzzer.play("MsgRcv3:d=4,o=6,b=200:32e,32g,32b,16c7");
    break;
  case UIEventType::channelMessage:
    buzzer.play("kerplop:d=16,o=6,b=120:32g#,32c#");
    break;
  case UIEventType::ack:
    buzzer.play("ack:d=32,o=8,b=120:c");
    break;
  case UIEventType::roomMessage:
  case UIEventType::newContactMessage:
  case UIEventType::none:
  default:
    break;
}
#endif

#ifdef PIN_VIBRATION
  // Trigger vibration for all UI events except none
  if (t != UIEventType::none) {
    vibration.trigger();
  }
#endif
}

void UITask::msgRead(int msgcount) {
  _msgcount = msgcount;
}

void UITask::newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) {
  _msgcount = msgcount;   // message itself is handled by onContactMsg() / onChannelMsg()
}

void UITask::wakeForMessage() {
  if (_display != NULL) {
    if (!_display->isOn() && !hasConnection()) {
      _display->turnOn();
    }
    if (_display->isOn()) {
      _auto_off = millis() + autoOffMillis();  // extend the auto-off timer
      _next_refresh = 100;  // trigger refresh
    }
  }
}

void UITask::onIncomingMsg(const ChatKey& key, const char* from_name) {
  UIScreen* c = curr();
  bool display_was_off = _display != NULL && !_display->isOn();
  if (c == chat && ((ChatScreen *) chat)->getKey() == key) {
    ((ChatScreen *) chat)->scrollToBottom();   // already looking at this conversation
  } else if (c == home || c == splash || display_was_off) {
    gotoHomeScreen();
    ((HomeScreen *) home)->showNewestMessage();   // show it right away, like a pager
  } else {
    char alert[48];
    snprintf(alert, sizeof(alert), "Msg: %s", from_name);
    showAlert(alert, 1500);
  }
  wakeForMessage();
}

void UITask::onContactMsg(const ContactInfo& from, uint32_t sender_timestamp, const char* text) {
  ChatKey key = ChatKey::dm(from.id.pub_key);
  _msgs.add(key, MSG_FLAG_UNREAD, rtc_clock.getCurrentTime(), from.name, text);
  onIncomingMsg(key, from.name);
}

void UITask::onChannelMsg(uint8_t channel_idx, uint32_t timestamp, const char* text) {
  ChatKey key = ChatKey::channel(channel_idx);
  _msgs.add(key, MSG_FLAG_UNREAD, rtc_clock.getCurrentTime(), "", text);
  char name[32];
  getChatName(key, name, sizeof(name));
  onIncomingMsg(key, name);
}

void UITask::onMsgAck(uint32_t ack) {
  UIMsg* m = _msgs.findPendingByAck(ack);
  if (m && !(m->flags & MSG_FLAG_DELIVERED)) {
    m->flags = (m->flags & ~(MSG_FLAG_PENDING | MSG_FLAG_FAILED)) | MSG_FLAG_DELIVERED;
    _next_refresh = 0;
  }
}

void UITask::userLedHandler() {
#ifdef PIN_STATUS_LED
  int cur_time = millis();
  if (cur_time > next_led_change) {
    if (led_state == 0) {
      led_state = 1;
      if (_msgs.unreadCount() > 0) {
        last_led_increment = LED_ON_MSG_MILLIS;
      } else {
        last_led_increment = LED_ON_MILLIS;
      }
      next_led_change = cur_time + last_led_increment;
    } else {
      led_state = 0;
      next_led_change = cur_time + LED_CYCLE_MILLIS - last_led_increment;
    }
    digitalWrite(PIN_STATUS_LED, led_state == LED_STATE_ON);
  }
#endif
}

/*
  hardware-agnostic pre-shutdown activity should be done here
*/
void UITask::shutdown(bool restart){
  flushSettings(false);

  #ifdef PIN_BUZZER
  buzzer.shutdown();
  uint32_t buzzer_timer = millis(); // fail-safe shutdown
  while (buzzer.isPlaying() && (millis() - 2500) < buzzer_timer)
    buzzer.loop();
  #endif // PIN_BUZZER

  if (restart) {
    _board->reboot();
  } else {
    // Power off board including radio, display, GPS and components
    _board->powerOff();
  }
}

bool UITask::isButtonPressed() const {
#ifdef PIN_USER_BTN
  return user_btn.isPressed();
#else
  return false;
#endif
}

// debounced joystick direction: one key on press, then auto-repeat while held
char UITask::pollJoy(JoyDir& j) {
  unsigned long now = millis();
  bool raw = j.btn->isPressed();
  if (raw != j.raw) {
    j.raw = raw;
    j.raw_at = now;
  }
  if (now - j.raw_at < JOY_DEBOUNCE_MILLIS) return 0;   // not stable yet

  if (!j.raw) {
    j.down = false;
    return 0;
  }
  if (!j.down) {
    j.down = true;
    j.next_repeat = now + JOY_REPEAT_DELAY_MILLIS;
    return j.key;
  }
  if ((long)(now - j.next_repeat) >= 0) {
    j.next_repeat = now + JOY_REPEAT_MILLIS;
    return j.key;
  }
  return 0;
}

void UITask::loop() {
  char c = 0;
#if UI_HAS_JOYSTICK
  int ev = user_btn.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_ENTER);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_CONTEXT_MENU);
  }
  for (int i = 0; i < 4 && c == 0; i++) {
    char k = pollJoy(_joy[i]);
    if (k) c = checkDisplayOn(k);
  }
  ev = back_btn.check();
  if (c == 0) {
    if (ev == BUTTON_EVENT_CLICK) {
      c = checkDisplayOn(KEY_CANCEL);
    } else if (ev == BUTTON_EVENT_DOUBLE_CLICK || ev == BUTTON_EVENT_LONG_PRESS) {
      c = checkDisplayOn(KEY_HOME);
    } else if (ev == BUTTON_EVENT_TRIPLE_CLICK) {
      checkDisplayOn(KEY_SELECT);
      toggleBuzzer();
    }
  }
#elif defined(PIN_USER_BTN)
  int ev = user_btn.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_NEXT);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_ENTER);
  } else if (ev == BUTTON_EVENT_DOUBLE_CLICK) {
    c = checkDisplayOn(KEY_CANCEL);
  }
#endif

  if (c != 0 && curr()) {
    if (c == KEY_HOME) {
      gotoHomeScreen();
    } else if (!curr()->handleInput(c) && c == KEY_CANCEL && curr() == home) {
      if (_display != NULL) _display->turnOff();   // Back on the home screen blanks the display
    }
    _auto_off = millis() + autoOffMillis();   // extend auto-off timer
    _next_refresh = 0;  // trigger refresh
  }

  userLedHandler();

#ifdef PIN_BUZZER
  if (buzzer.isPlaying())  buzzer.loop();
#endif

  if (millis() >= next_timeout_chck) {
    if (_msgs.checkTimeouts(millis())) _next_refresh = 0;
    next_timeout_chck = millis() + 1000;
  }

  if (curr()) curr()->poll();

  if (_display != NULL && _display->isOn()) {
    if (millis() >= _next_refresh && curr()) {
      renderFrame();
    }
#ifdef KEEP_DISPLAY_ON_USB
    if (board.isExternalPowered()) {
      _auto_off = millis() + autoOffMillis();
    }
#endif
    if (millis() > _auto_off) {
      flushSettings(false);   // don't leave unsaved settings behind a dark screen
      _display->turnOff();
    }
  }

#ifdef PIN_VIBRATION
  vibration.loop();
#endif

#ifdef AUTO_SHUTDOWN_MILLIVOLTS
  if (millis() > next_batt_chck) {
    uint16_t milliVolts = getBattMilliVolts();
    if (milliVolts > 0 && milliVolts < AUTO_SHUTDOWN_MILLIVOLTS) {
      if(!board.isExternalPowered()) {
        if (_display != NULL) {
          _display->startFrame();
          _display->setTextSize(2);
          _display->setColor(UIColor::warning_txt);
          _display->drawTextCentered(_display->width() / 2, 20, "Low Battery.");
          _display->drawTextCentered(_display->width() / 2, 40, "Shutting Down!");
          _display->endFrame();
          if (_display->isEink() == false) { delay(3000); }
        }
        shutdown();
      }
    }
    next_batt_chck = millis() + 8000;
  }
#endif
}

void UITask::renderFrame() {
  _display->startFrame();
  int delay_millis = curr()->render(*_display);
  if (millis() < _alert_expiry) {  // render alert popup
    _display->setTextSize(1);
    int y = _display->height() / 3;
    int p = _display->height() / 32;
    _display->setColor(UIColor::popup_bkg);
    _display->fillRect(p, y, _display->width() - p*2, y);
    _display->setColor(UIColor::popup_txt);  // draw box border
    _display->drawRect(p, y, _display->width() - p*2, y);
    _display->drawTextCentered(_display->width() / 2, y + p*3, _alert);
    _next_refresh = _alert_expiry;   // will need refresh when alert is dismissed
  } else {
    _next_refresh = millis() + delay_millis;
  }
  _display->endFrame();
}

char UITask::checkDisplayOn(char c) {
  if (_display != NULL) {
    if (!_display->isOn()) {
      _display->turnOn();   // turn display on and consume event
      c = 0;
    }
    _auto_off = millis() + autoOffMillis();   // extend auto-off timer
    _next_refresh = 0;  // trigger refresh
  }
  return c;
}

char UITask::handleLongPress(char c) {
  if (millis() - ui_started_at < 8000) {   // long press in first 8 seconds since startup -> CLI/rescue
    the_mesh.enterCLIRescue();
    c = 0;   // consume event
  } else {
    c = checkDisplayOn(c);
  }
  return c;
}

bool UITask::getGPSState() {
  if (_sensors != NULL) {
    int num = _sensors->getNumSettings();
    for (int i = 0; i < num; i++) {
      if (strcmp(_sensors->getSettingName(i), "gps") == 0) {
        return !strcmp(_sensors->getSettingValue(i), "1");
      }
    }
  }
  return false;
}

void UITask::toggleGPS(bool from_settings) {
    if (_sensors != NULL) {
    // toggle GPS on/off
    int num = _sensors->getNumSettings();
    for (int i = 0; i < num; i++) {
      if (strcmp(_sensors->getSettingName(i), "gps") == 0) {
        if (strcmp(_sensors->getSettingValue(i), "1") == 0) {
          _sensors->setSettingValue("gps", "0");
          _node_prefs->gps_enabled = 0;
          notify(UIEventType::ack);
        } else {
          _sensors->setSettingValue("gps", "1");
          _node_prefs->gps_enabled = 1;
          notify(UIEventType::ack);
        }
        if (from_settings) {
          markNodePrefsDirty();   // saved when leaving Settings
        } else {
          the_mesh.savePrefs();
          showAlert(_node_prefs->gps_enabled ? "GPS: Enabled" : "GPS: Disabled", 800);
        }
        _next_refresh = 0;
        break;
      }
    }
  }
}

void UITask::toggleBuzzer(bool from_settings) {
    // Toggle buzzer quiet mode
  #ifdef PIN_BUZZER
    if (buzzer.isQuiet()) {
      buzzer.quiet(false);
      notify(UIEventType::ack);
    } else {
      buzzer.quiet(true);
    }
    _node_prefs->buzzer_quiet = buzzer.isQuiet();
    if (from_settings) {
      markNodePrefsDirty();   // saved when leaving Settings
    } else {
      the_mesh.savePrefs();
      showAlert(buzzer.isQuiet() ? "Buzzer: OFF" : "Buzzer: ON", 800);
    }
    _next_refresh = 0;  // trigger refresh
  #endif
}
