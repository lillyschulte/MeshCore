#pragma once

#include "Widgets.h"
#include "../MyMesh.h"
#include "target.h"

#ifndef MAX_CONTACTS
  #define MAX_CONTACTS 100
#endif
#ifndef MAX_GROUP_CHANNELS
  #define MAX_GROUP_CHANNELS 8
#endif

/* ---------------------------------------------------------------- Main menu */

class MainMenuScreen : public ListScreen {
  enum Item { MESSAGES, CONTACTS, CHANNELS, ADVERT, SETTINGS, BLUETOOTH, SHUTDOWN, Count };
  bool _shutdown_init;

protected:
  const char* getTitle() override { return "Menu"; }
  int getCount() override { return Item::Count; }

  void getItem(int i, char* label, int label_size, char* value, int value_size) override {
    switch (i) {
      case MESSAGES: {
        strcpy(label, "Messages");
        int unread = _task->msgs().unreadCount();
        if (unread > 0) snprintf(value, value_size, "%d new", unread);
        break;
      }
      case CONTACTS:  strcpy(label, "Contacts"); break;
      case CHANNELS:  strcpy(label, "Channels"); break;
      case ADVERT:    strcpy(label, "Send advert"); break;
      case SETTINGS:  strcpy(label, "Settings"); break;
      case BLUETOOTH:
        strcpy(label, "Bluetooth");
        strcpy(value, _task->isBluetoothEnabled() ? "On" : "Off");
        break;
      case SHUTDOWN:  strcpy(label, _shutdown_init ? "Hibernating..." : "Shutdown"); break;
    }
  }

  void onEnter(int i) override {
    switch (i) {
      case MESSAGES: _task->openThreads(); break;
      case CONTACTS: _task->openContacts(); break;
      case CHANNELS: _task->openChannels(); break;
      case ADVERT:
        _task->notify(UIEventType::ack);
        _task->showAlert(the_mesh.advert() ? "Advert sent!" : "Advert failed..", 1000);
        break;
      case SETTINGS: _task->openSettings(); break;
      case BLUETOOTH:
        if (_task->isBluetoothEnabled()) {
          _task->disableBluetooth();
        } else {
          _task->enableBluetooth();
        }
        break;
      case SHUTDOWN:
        _shutdown_init = true;   // wait for the button to be released
        break;
    }
  }

public:
  MainMenuScreen(UITask* task) : ListScreen(task), _shutdown_init(false) { }

  void poll() override {
    if (_shutdown_init && !_task->isButtonPressed()) {
      _task->shutdown();
    }
  }
};

/* ------------------------------------------------------------ Conversations */

#define UI_MAX_THREADS  24

class ThreadsScreen : public ListScreen {
  ChatKey _threads[UI_MAX_THREADS];
  int _num;

  void refresh() { _num = _task->msgs().getThreads(_threads, UI_MAX_THREADS); }

protected:
  const char* getTitle() override { return "Messages"; }
  const char* getEmptyText() override { return "No messages yet"; }
  int getCount() override { refresh(); return _num; }

  void getItem(int i, char* label, int label_size, char* value, int value_size) override {
    char name[32];
    _task->getChatName(_threads[i], name, sizeof(name));
    int unread = _task->msgs().unreadCount(_threads[i]);
    snprintf(label, label_size, "%s%s", unread > 0 ? "*" : "", name);
    if (unread > 0) {
      snprintf(value, value_size, "%d", unread);
    } else {
      UIMsg* m = _task->msgs().latestFor(_threads[i]);
      if (m && _task->isClockSet()) formatAge(value, rtc_clock.getCurrentTime() - m->timestamp);
    }
  }

  void onEnter(int i) override { _task->openChat(_threads[i]); }

public:
  ThreadsScreen(UITask* task) : ListScreen(task), _num(0) { }
};

/* ----------------------------------------------------------------- Contacts */

class ContactsScreen : public ListScreen {
  struct Entry {
    uint32_t lastmod;
    uint16_t idx;
  };
  Entry _entries[MAX_CONTACTS];
  int _num;

  static int sortByRecent(const void* a, const void* b) {
    uint32_t la = ((const Entry*) a)->lastmod, lb = ((const Entry*) b)->lastmod;
    return la < lb ? 1 : (la > lb ? -1 : 0);
  }

  static const char* typeTag(uint8_t type) {
    switch (type) {
      case ADV_TYPE_REPEATER: return "Rpt";
      case ADV_TYPE_ROOM:     return "Room";
      case ADV_TYPE_SENSOR:   return "Sens";
      default:                return "";
    }
  }

protected:
  const char* getTitle() override { return "Contacts"; }
  const char* getEmptyText() override { return "No contacts"; }
  int getCount() override { return _num; }

  void getItem(int i, char* label, int label_size, char* value, int value_size) override {
    ContactInfo c;
    if (!the_mesh.getContactByIdx(_entries[i].idx, c)) return;
    StrHelper::strncpy(label, c.name, label_size);
    const char* tag = typeTag(c.type);
    if (tag[0]) {
      strcpy(value, tag);
    } else if (_task->isClockSet() && c.lastmod > 0) {
      formatAge(value, rtc_clock.getCurrentTime() - c.lastmod);
    }
  }

  void onEnter(int i) override {
    ContactInfo c;
    if (!the_mesh.getContactByIdx(_entries[i].idx, c)) return;
    if (c.type == ADV_TYPE_CHAT || c.type == ADV_TYPE_ROOM) {
      _task->openChat(ChatKey::dm(c.id.pub_key));
    } else {
      _task->showAlert("Not a chat node", 1000);
    }
  }

public:
  ContactsScreen(UITask* task) : ListScreen(task), _num(0) { }

  void refresh() {
    _num = 0;
    int n = the_mesh.getTotalContactSlots();   // raw slots, the first ones are reserved (anon)
    ContactInfo c;
    for (int i = 0; i < n && _num < MAX_CONTACTS; i++) {
      if (the_mesh.getContactByIdx(i, c) && c.type != ADV_TYPE_NONE) {
        _entries[_num].lastmod = c.lastmod;
        _entries[_num].idx = i;
        _num++;
      }
    }
    qsort(_entries, _num, sizeof(Entry), sortByRecent);
    reset();
  }
};

/* ----------------------------------------------------------------- Channels */

class ChannelsScreen : public ListScreen {
  uint8_t _idx[MAX_GROUP_CHANNELS];
  int _num;

protected:
  const char* getTitle() override { return "Channels"; }
  const char* getEmptyText() override { return "No channels"; }
  int getCount() override { return _num; }

  void getItem(int i, char* label, int label_size, char* value, int value_size) override {
    ChannelDetails ch;
    if (!the_mesh.getChannel(_idx[i], ch)) return;
    StrHelper::strncpy(label, ch.name, label_size);
    int unread = _task->msgs().unreadCount(ChatKey::channel(_idx[i]));
    if (unread > 0) snprintf(value, value_size, "%d", unread);
  }

  void onEnter(int i) override { _task->openChat(ChatKey::channel(_idx[i])); }

public:
  ChannelsScreen(UITask* task) : ListScreen(task), _num(0) { }

  void refresh() {
    _num = 0;
    ChannelDetails ch;
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
      if (the_mesh.getChannel(i, ch) && ch.name[0] != 0) {
        _idx[_num++] = i;
      }
    }
    reset();
  }
};

/* --------------------------------------------------------------------- Chat */

#define CHAT_COLS   21   // 128 px / 6 px per char
#define CHAT_ROWS    6

class ChatScreen : public UIScreen {
  UITask* _task;
  ChatKey _key;
  int _scroll;   // lines scrolled up from the bottom (newest)
  int _total_lines;

  // build the display text of a message: "Bob: hi", "Me: hi \xFB"
  static void formatMsg(DisplayDriver& display, UIMsg* m, char* dest, int dest_size) {
    char raw[UI_MSG_TEXT_LEN + UI_MSG_SENDER_LEN + 8];
    const char* status = "";
    if (m->flags & MSG_FLAG_OUT) {
      if (m->flags & MSG_FLAG_DELIVERED) status = " \xFB";       // CP437 check mark
      else if (m->flags & MSG_FLAG_FAILED) status = " !";
      else if (m->flags & MSG_FLAG_PENDING) status = " ..";
      snprintf(raw, sizeof(raw), "Me: %s", m->text);
    } else if (m->sender[0]) {
      snprintf(raw, sizeof(raw), "%s: %s", m->sender, m->text);
    } else {
      snprintf(raw, sizeof(raw), "%s", m->text);   // channel msgs already carry "name: "
    }
    display.translateUTF8ToBlocks(dest, raw, dest_size - 4);
    strcat(dest, status);
  }

public:
  ChatScreen(UITask* task) : _task(task), _scroll(0), _total_lines(0) {
    _key.kind = CHAT_KIND_NONE;
  }

  void open(const ChatKey& key) {
    _key = key;
    _scroll = 0;
  }
  const ChatKey& getKey() const { return _key; }
  void scrollToBottom() { _scroll = 0; }

  int render(DisplayDriver& display) override {
    char name[32];
    _task->getChatName(_key, name, sizeof(name));
    drawTitleBar(display, _task, name);

    MsgStore& store = _task->msgs();
    store.markRead(_key);

    // pass 1: count lines
    char buf[UI_MSG_TEXT_LEN + UI_MSG_SENDER_LEN + 12];
    int total = 0;
    for (int i = 0; i < store.count(); i++) {
      UIMsg* m = store.get(i);
      if (!(m->key == _key)) continue;
      formatMsg(display, m, buf, sizeof(buf));
      total += wrapText(buf, CHAT_COLS, [](const char*, int) { });
    }
    _total_lines = total;

    if (total == 0) {
      display.setColor(UIColor::secondary_txt);
      display.drawTextCentered(display.width() / 2, 26, "No messages");
      display.drawTextCentered(display.width() / 2, 40, "Enter: write");
      return 5000;
    }

    int max_scroll = total > CHAT_ROWS ? total - CHAT_ROWS : 0;
    if (_scroll > max_scroll) _scroll = max_scroll;
    if (_scroll < 0) _scroll = 0;
    int first = total - CHAT_ROWS - _scroll;   // index of the first visible line (may be < 0)

    // pass 2: draw visible lines; outgoing messages are drawn indented by a marker bar
    int line = 0;
    display.setTextSize(1);
    display.setColor(UIColor::primary_txt);
    for (int i = 0; i < store.count(); i++) {
      UIMsg* m = store.get(i);
      if (!(m->key == _key)) continue;
      formatMsg(display, m, buf, sizeof(buf));
      bool out = (m->flags & MSG_FLAG_OUT) != 0;
      wrapText(buf, CHAT_COLS, [&](const char* s, int len) {
        int vis = line - first;
        if (vis >= 0 && vis < CHAT_ROWS) {
          char seg[CHAT_COLS + 1];
          memcpy(seg, s, len);
          seg[len] = 0;
          int y = UI_LIST_TOP + vis * UI_ROW_H;
          if (out) display.fillRect(0, y, 1, UI_ROW_H - 1);   // marker for own messages
          display.setCursor(2, y);
          display.print(seg);
        }
        line++;
      });
    }
    drawScrollBar(display, max_scroll - _scroll, CHAT_ROWS, total);
    return 2000;
  }

  bool handleInput(char c) override {
    if (c == KEY_UP || c == KEY_PREV) {
      _scroll++;
      return true;
    }
    if (c == KEY_DOWN || c == KEY_NEXT) {
      if (_scroll > 0) _scroll--;
      return true;
    }
    if (c == KEY_ENTER || c == KEY_RIGHT) {
      _task->openReply(_key);
      return true;
    }
    if (c == KEY_LEFT || c == KEY_CANCEL) {
      _task->pop();
      return true;
    }
    return false;
  }
};

/* -------------------------------------------------------------------- Reply */

class ReplyScreen : public ListScreen, public KeyboardListener {
  ChatKey _key;
  bool _offer_open_chat;   // when opened from the home messages page
  int _canned_idx[UI_CANNED_COUNT];
  int _num_canned;

  void refresh() {
    _num_canned = 0;
    for (int i = 0; i < UI_CANNED_COUNT; i++) {
      if (_task->uiPrefs().canned[i][0]) _canned_idx[_num_canned++] = i;
    }
  }
  int fixedItems() const { return _offer_open_chat ? 2 : 1; }   // [Open chat], Write...

protected:
  const char* getTitle() override { return "Reply"; }
  int getCount() override { return fixedItems() + _num_canned; }

  void getItem(int i, char* label, int label_size, char* value, int value_size) override {
    if (_offer_open_chat && i == 0) {
      strcpy(label, "Open chat");
    } else if (i == fixedItems() - 1) {
      strcpy(label, "Write...");
    } else {
      StrHelper::strncpy(label, _task->uiPrefs().canned[_canned_idx[i - fixedItems()]], label_size);
    }
  }

  void onEnter(int i) override {
    ChatKey key = _key;
    if (_offer_open_chat && i == 0) {
      _task->openChat(key);
    } else if (i == fixedItems() - 1) {
      _task->openKeyboard("Message", "", _task->getMaxComposeLen(key), this, 0);
    } else {
      _task->pop();   // back to where the reply was started
      _task->sendText(key, _task->uiPrefs().canned[_canned_idx[i - fixedItems()]]);
    }
  }

public:
  ReplyScreen(UITask* task) : ListScreen(task), _offer_open_chat(false), _num_canned(0) { }

  void open(const ChatKey& key, bool offer_open_chat) {
    _key = key;
    _offer_open_chat = offer_open_chat;
    refresh();
    reset();
  }

  void onKeyboardDone(int tag, const char* text) override {
    ChatKey key = _key;
    _task->pop();   // reply screen -> where the reply was started
    if (text[0]) _task->sendText(key, text);
  }
};

/* ----------------------------------------------------------------- Settings */

class SettingsScreen : public ListScreen, public KeyboardListener {
  enum Item {
    NAME,
#ifdef PIN_BUZZER
    BUZZER,
#endif
#if ENV_INCLUDE_GPS == 1
    GPS,
#endif
    TIMEZONE,
    CLOCK_FMT,
    SCREEN_OFF,
    BATTERY,
    CANNED,
    RADIO_INFO,
    Count
  };
  static const uint16_t* timeouts(int& n) {
    static const uint16_t t[] = { 10, 15, 30, 60, 120, 300 };
    n = sizeof(t) / sizeof(t[0]);
    return t;
  }

protected:
  const char* getTitle() override { return "Settings"; }
  int getCount() override { return Item::Count; }

  void getItem(int i, char* label, int label_size, char* value, int value_size) override {
    UIPrefs& p = _task->uiPrefs();
    switch (i) {
      case NAME:
        strcpy(label, "Name");
        StrHelper::strncpy(value, _task->nodePrefs()->node_name, value_size > 12 ? 12 : value_size);
        break;
#ifdef PIN_BUZZER
      case BUZZER:
        strcpy(label, "Buzzer");
        strcpy(value, _task->isBuzzerQuiet() ? "Off" : "On");
        break;
#endif
#if ENV_INCLUDE_GPS == 1
      case GPS:
        strcpy(label, "GPS");
        strcpy(value, _task->getGPSState() ? "On" : "Off");
        break;
#endif
      case TIMEZONE: {
        strcpy(label, "Time zone");
        int off = p.tz_offset_min;
        char sign = off < 0 ? '-' : '+';
        if (off < 0) off = -off;
        snprintf(value, value_size, "UTC%c%d:%02d", sign, off / 60, off % 60);
        break;
      }
      case CLOCK_FMT:
        strcpy(label, "Clock");
        strcpy(value, p.clock_24h ? "24h" : "12h");
        break;
      case SCREEN_OFF:
        strcpy(label, "Screen off");
        if (p.screen_timeout_s >= 60) snprintf(value, value_size, "%dm", p.screen_timeout_s / 60);
        else snprintf(value, value_size, "%ds", p.screen_timeout_s);
        break;
      case BATTERY:
        strcpy(label, "Battery");
        strcpy(value, p.batt_percent ? "%" : "Icon");
        break;
      case CANNED:
        strcpy(label, "Quick replies");
        strcpy(value, ">");
        break;
      case RADIO_INFO: {
        NodePrefs* np = _task->nodePrefs();
        snprintf(label, label_size, "%.3f SF%d", np->freq, np->sf);
        snprintf(value, value_size, "BW%.0f", np->bw);
        break;
      }
    }
  }

  bool onLeftRight(int i, int dir) override {
    UIPrefs& p = _task->uiPrefs();
    switch (i) {
      case TIMEZONE:
        p.tz_offset_min += dir * 15;
        if (p.tz_offset_min < -12*60) p.tz_offset_min = 14*60;
        if (p.tz_offset_min > 14*60) p.tz_offset_min = -12*60;
        _task->markUIPrefsDirty();
        return true;
      case CLOCK_FMT:
        p.clock_24h = !p.clock_24h;
        _task->markUIPrefsDirty();
        return true;
      case BATTERY:
        p.batt_percent = !p.batt_percent;
        _task->markUIPrefsDirty();
        return true;
      case SCREEN_OFF: {
        int n;
        const uint16_t* t = timeouts(n);
        int cur = 0;
        for (int k = 0; k < n; k++) if (t[k] == p.screen_timeout_s) cur = k;
        cur = (cur + n + dir) % n;
        p.screen_timeout_s = t[cur];
        _task->markUIPrefsDirty();
        return true;
      }
#ifdef PIN_BUZZER
      case BUZZER:
        _task->toggleBuzzer(true);
        return true;
#endif
#if ENV_INCLUDE_GPS == 1
      case GPS:
        _task->toggleGPS(true);
        return true;
#endif
    }
    return false;
  }

  void onEnter(int i) override {
    switch (i) {
      case NAME:
        _task->openKeyboard("Node name", _task->nodePrefs()->node_name,
                            sizeof(_task->nodePrefs()->node_name) - 1, this, NAME);
        break;
      case CANNED:
        _task->openCannedEdit();
        break;
      case RADIO_INFO:
        _task->showAlert("Change radio via app", 1200);
        break;
      default:
        onLeftRight(i, 1);   // Enter cycles values forward
        break;
    }
  }

public:
  SettingsScreen(UITask* task) : ListScreen(task) { }

  void onKeyboardDone(int tag, const char* text) override {
    if (tag == NAME && text[0]) {
      NodePrefs* np = _task->nodePrefs();
      StrHelper::strncpy(np->node_name, text, sizeof(np->node_name));
      _task->markNodePrefsDirty();
    }
  }
};

/* ------------------------------------------------------- Quick reply editor */

#define CANNED_TAG_ADD   100   // keyboard tag for a new reply (edits use the slot index)

class CannedEditScreen : public ListScreen, public KeyboardListener {
  enum PopupItem { POPUP_EDIT, POPUP_DELETE, POPUP_CANCEL, POPUP_COUNT };
  bool _popup;
  int _popup_sel;

  int numReplies() {
    int n = 0;
    while (n < UI_CANNED_COUNT && _task->uiPrefs().canned[n][0]) n++;
    return n;
  }

  // keep replies packed at the front, so "empty" only ever means "end of list"
  void compact() {
    UIPrefs& p = _task->uiPrefs();
    int dst = 0;
    for (int i = 0; i < UI_CANNED_COUNT; i++) {
      if (p.canned[i][0]) {
        if (dst != i) memcpy(p.canned[dst], p.canned[i], UI_CANNED_LEN);
        dst++;
      }
    }
    for (; dst < UI_CANNED_COUNT; dst++) p.canned[dst][0] = 0;
  }

  void deleteReply(int i) {
    _task->uiPrefs().canned[i][0] = 0;
    compact();
    _task->markUIPrefsDirty();
    _task->showAlert("Deleted", 700);
  }

protected:
  const char* getTitle() override { return "Quick replies"; }
  int getCount() override { return numReplies() + 1; }   // + "Add new"

  void getItem(int i, char* label, int label_size, char* value, int value_size) override {
    int n = numReplies();
    if (i < n) {
      StrHelper::strncpy(label, _task->uiPrefs().canned[i], label_size);
    } else {
      strcpy(label, "+ Add new");
      if (n >= UI_CANNED_COUNT) strcpy(value, "(full)");
    }
  }

  void onEnter(int i) override {
    int n = numReplies();
    if (i < n) {
      _popup = true;
      _popup_sel = POPUP_EDIT;
    } else if (n >= UI_CANNED_COUNT) {
      _task->showAlert("Max 8 replies", 900);
    } else {
      _task->openKeyboard("New quick reply", "", UI_CANNED_LEN - 1, this, CANNED_TAG_ADD);
    }
  }

public:
  CannedEditScreen(UITask* task) : ListScreen(task), _popup(false), _popup_sel(0) { }

  void open() {
    compact();   // tidy up files from older versions that had gaps
    _popup = false;
    reset();
  }

  int render(DisplayDriver& display) override {
    int next = ListScreen::render(display);
    if (!_popup) return next;

    // Edit / Delete / Cancel popup over the list
    static const char* const labels[POPUP_COUNT] = { "Edit", "Delete", "Cancel" };
    const int w = 60, row_h = 10;
    const int h = POPUP_COUNT * row_h + 4;
    int x = (display.width() - w) / 2;
    int y = (display.height() - h) / 2 + 4;
    display.setColor(UIColor::window_bkg);
    display.fillRect(x - 1, y - 1, w + 2, h + 2);
    display.setColor(UIColor::primary_txt);
    display.drawRect(x, y, w, h);
    for (int k = 0; k < POPUP_COUNT; k++) {
      int ry = y + 2 + k * row_h;
      if (k == _popup_sel) {
        display.setColor(UIColor::primary_txt);
        display.fillRect(x + 2, ry, w - 4, row_h);
        display.setColor(UIColor::window_bkg);
      } else {
        display.setColor(UIColor::primary_txt);
      }
      display.drawTextCentered(display.width() / 2, ry + 1, labels[k]);
    }
    display.setColor(UIColor::primary_txt);
    return next;
  }

  bool handleInput(char c) override {
    if (!_popup) return ListScreen::handleInput(c);

    switch (c) {
      case KEY_UP:
        _popup_sel = (_popup_sel + POPUP_COUNT - 1) % POPUP_COUNT;
        return true;
      case KEY_DOWN:
        _popup_sel = (_popup_sel + 1) % POPUP_COUNT;
        return true;
      case KEY_ENTER:
      case KEY_RIGHT:
        _popup = false;
        if (_popup_sel == POPUP_EDIT) {
          _task->openKeyboard("Quick reply", _task->uiPrefs().canned[_sel], UI_CANNED_LEN - 1, this, _sel);
        } else if (_popup_sel == POPUP_DELETE) {
          deleteReply(_sel);
        }
        return true;
      case KEY_LEFT:
      case KEY_CANCEL:
        _popup = false;
        return true;
    }
    return true;   // popup is modal
  }

  void onKeyboardDone(int tag, const char* text) override {
    UIPrefs& p = _task->uiPrefs();
    if (tag == CANNED_TAG_ADD) {
      int n = numReplies();
      if (text[0] && n < UI_CANNED_COUNT) {
        StrHelper::strncpy(p.canned[n], text, UI_CANNED_LEN);
        _sel = n;
      }
    } else if (tag >= 0 && tag < UI_CANNED_COUNT) {
      StrHelper::strncpy(p.canned[tag], text, UI_CANNED_LEN);   // clearing the text deletes it
      compact();
    }
    _task->markUIPrefsDirty();
  }
};
