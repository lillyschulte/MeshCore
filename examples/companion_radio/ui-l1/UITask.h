#pragma once

#include <MeshCore.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/ui/UIScreen.h>
#include <helpers/ui/MomentaryButton.h>
#include <helpers/SensorManager.h>
#include <helpers/MultiSerialInterface.h>
#include <Arduino.h>
#include <helpers/sensors/LPPDataHelpers.h>

#ifndef LED_STATE_ON
  #define LED_STATE_ON 1
#endif

#ifdef PIN_BUZZER
  #include <helpers/ui/buzzer.h>
#endif
#ifdef PIN_VIBRATION
  #include <helpers/ui/GenericVibration.h>
#endif

#include "../AbstractUITask.h"
#include "../NodePrefs.h"
#include "MsgStore.h"
#include "UIPrefs.h"

// the on-screen keyboard reports its result through this
class KeyboardListener {
public:
  virtual void onKeyboardDone(int tag, const char* text) = 0;
};

#define UI_NAV_STACK_SIZE   8
#define UI_MAX_COMPOSE_LEN  140

class UITask : public AbstractUITask {
  DisplayDriver* _display;
  SensorManager* _sensors;
#ifdef PIN_BUZZER
  genericBuzzer buzzer;
#endif
#ifdef PIN_VIBRATION
  GenericVibration vibration;
#endif
  unsigned long _next_refresh, _auto_off;
  NodePrefs* _node_prefs;
  UIPrefs _ui_prefs;
  MsgStore _msgs;
  char _alert[80];
  unsigned long _alert_expiry;
  int _msgcount;
  unsigned long ui_started_at, next_batt_chck, next_timeout_chck;
#ifdef PIN_STATUS_LED
  int led_state = 0;
  int next_led_change = 0;
  int last_led_increment = 0;
#endif

  // joystick directions, polled directly so they can auto-repeat while held
  struct JoyDir {
    MomentaryButton* btn;
    char key;
    bool raw, down;
    unsigned long raw_at, next_repeat;
  };
  JoyDir _joy[4];
  char pollJoy(JoyDir& j);

  UIScreen* splash;
  UIScreen* home;
  UIScreen* menu;
  UIScreen* threads;
  UIScreen* contacts;
  UIScreen* channels;
  UIScreen* chat;
  UIScreen* reply;
  UIScreen* settings;
  UIScreen* canned_edit;
  UIScreen* keyboard;
  UIScreen* _stack[UI_NAV_STACK_SIZE];
  int _stack_len;

  void userLedHandler();

  // Button action handlers
  char checkDisplayOn(char c);
  char handleLongPress(char c);

  void setCurrScreen(UIScreen* c);
  UIScreen* curr() const { return _stack_len > 0 ? _stack[_stack_len - 1] : NULL; }
  unsigned long autoOffMillis() const { return (unsigned long)_ui_prefs.screen_timeout_s * 1000UL; }
  void wakeForMessage();
  void onIncomingMsg(const ChatKey& key, const char* from_name);

public:

  UITask(mesh::MainBoard* board, MultiSerialInterface* serial) : AbstractUITask(board, serial), _display(NULL), _sensors(NULL) {
    next_batt_chck = next_timeout_chck = _next_refresh = 0;
    ui_started_at = 0;
    _stack_len = 0;
    _msgcount = 0;
  }
  void begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs);

  // navigation
  void gotoHomeScreen();
  void push(UIScreen* s);
  void pop();
  void openMenu();
  void openThreads();
  void openContacts();
  void openChannels();
  void openSettings();
  void openCannedEdit();
  void openChat(const ChatKey& key);
  void openReply(const ChatKey& key, bool offer_open_chat = false);
  void openKeyboard(const char* title, const char* initial, int max_len, KeyboardListener* listener, int tag);
  void requestRefresh() { _next_refresh = 0; }

  void showAlert(const char* text, int duration_millis);
  int  getMsgCount() const { return _msgcount; }
  bool hasDisplay() const { return _display != NULL; }
  bool isButtonPressed() const;

  // data access for screens
  MsgStore& msgs() { return _msgs; }
  UIPrefs& uiPrefs() { return _ui_prefs; }
  void saveUIPrefs();
  NodePrefs* nodePrefs() { return _node_prefs; }
  void getChatName(const ChatKey& key, char* dest, int dest_size);
  int  getMaxComposeLen(const ChatKey& key) const;
  bool sendText(const ChatKey& key, const char* text);

  // clock helpers (local time = RTC + time zone offset)
  bool isClockSet() const;
  uint32_t getLocalTime() const;
  void formatClock(uint32_t local_time, char* dest, bool with_ampm = true) const;

  bool isBuzzerQuiet() {
#ifdef PIN_BUZZER
    return buzzer.isQuiet();
#else
    return true;
#endif
  }

  void toggleBuzzer();
  bool getGPSState();
  void toggleGPS();

  // from AbstractUITask
  void msgRead(int msgcount) override;
  void newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount) override;
  void notify(UIEventType t = UIEventType::none) override;
  void loop() override;
  void onContactMsg(const ContactInfo& from, uint32_t sender_timestamp, const char* text) override;
  void onChannelMsg(uint8_t channel_idx, uint32_t timestamp, const char* text) override;
  void onMsgAck(uint32_t ack) override;

  void shutdown(bool restart = false);
};
