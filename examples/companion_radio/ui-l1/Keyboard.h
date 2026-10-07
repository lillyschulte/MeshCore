#pragma once

#include "Widgets.h"

#define KB_COLS        10
#define KB_CHAR_ROWS    4
#define KB_ROWS        (KB_CHAR_ROWS + 1)   // + special keys row
#define KB_SPECIALS     4
#define KB_CELL_W      12
#define KB_CELL_H      10
#define KB_GRID_X       4
#define KB_GRID_Y      11

enum KbLayer { KB_LOWER, KB_UPPER, KB_SYMBOLS, KB_NUM_LAYERS };
enum KbSpecial { KB_SHIFT, KB_SPACE, KB_DEL, KB_OK };

static const char* const kb_layers[KB_NUM_LAYERS][KB_CHAR_ROWS] = {
  { "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?" },
  { "1234567890", "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM;:!" },
  { "!@#$%^&*()", "-_=+[]{}<>", "/\\|~`;:'\"?", ",.!?#@&*+-" },
};

// joystick-driven on-screen keyboard. Enter types the key under the cursor,
// long-press Enter or OK finishes, Back cancels.
class KeyboardScreen : public UIScreen {
  UITask* _task;
  KeyboardListener* _listener;
  int _tag;
  char _title[24];
  char _text[UI_MAX_COMPOSE_LEN + 1];
  int _max_len;
  uint8_t _layer;
  bool _one_shot_upper;   // drop back to lower case after one capital
  int8_t _row, _col;

  void typeChar(char ch) {
    int len = strlen(_text);
    if (len >= _max_len) {
      _task->showAlert("Text full", 600);
      return;
    }
    _text[len] = ch;
    _text[len + 1] = 0;
    if (_layer == KB_UPPER && _one_shot_upper) _layer = KB_LOWER;
  }

  void finish(bool ok) {
    KeyboardListener* l = _listener;
    int tag = _tag;
    _listener = NULL;
    _task->pop();   // pop first, the listener may navigate further
    if (ok && l) l->onKeyboardDone(tag, _text);
  }

  void pressSpecial(int k) {
    switch (k) {
      case KB_SHIFT:
        if (_layer == KB_LOWER) { _layer = KB_UPPER; _one_shot_upper = true; }
        else if (_layer == KB_UPPER && _one_shot_upper) { _one_shot_upper = false; }   // second press = caps lock
        else if (_layer == KB_UPPER) { _layer = KB_SYMBOLS; }
        else { _layer = KB_LOWER; }
        break;
      case KB_SPACE:
        typeChar(' ');
        break;
      case KB_DEL: {
        int len = strlen(_text);
        if (len > 0) _text[len - 1] = 0;
        break;
      }
      case KB_OK:
        finish(true);
        break;
    }
  }

  const char* shiftLabel() const {
    if (_layer == KB_LOWER) return "abc";
    if (_layer == KB_UPPER) return _one_shot_upper ? "Abc" : "ABC";
    return "#+=";
  }

public:
  KeyboardScreen(UITask* task) : _task(task), _listener(NULL), _tag(0), _max_len(0) {
    _title[0] = _text[0] = 0;
  }

  void start(const char* title, const char* initial, int max_len, KeyboardListener* listener, int tag) {
    StrHelper::strncpy(_title, title, sizeof(_title));
    if (max_len > UI_MAX_COMPOSE_LEN) max_len = UI_MAX_COMPOSE_LEN;
    _max_len = max_len;
    StrHelper::strncpy(_text, initial ? initial : "", sizeof(_text));
    if ((int) strlen(_text) > _max_len) _text[_max_len] = 0;
    _listener = listener;
    _tag = tag;
    _layer = KB_LOWER;
    _one_shot_upper = false;
    _row = 1;
    _col = 0;
  }

  int render(DisplayDriver& display) override {
    display.setTextSize(1);

    // input line: show the tail of the text, or the title as a placeholder
    display.setColor(UIColor::primary_txt);
    int len = strlen(_text);
    if (len == 0) {
      display.setColor(UIColor::secondary_txt);
      display.setCursor(0, 0);
      display.print(_title);
    } else {
      const int max_vis = display.width() / 6 - 1;
      char filtered[UI_MAX_COMPOSE_LEN + 1];
      display.translateUTF8ToBlocks(filtered, _text, sizeof(filtered));
      int flen = strlen(filtered);
      const char* tail = flen > max_vis ? &filtered[flen - max_vis] : filtered;
      display.setCursor(0, 0);
      display.print(tail);
      display.print("_");
    }
    display.fillRect(0, 9, display.width(), 1);

    // character grid
    char ch[2] = { 0, 0 };
    for (int r = 0; r < KB_CHAR_ROWS; r++) {
      const char* row = kb_layers[_layer][r];
      for (int c = 0; c < KB_COLS; c++) {
        int x = KB_GRID_X + c * KB_CELL_W;
        int y = KB_GRID_Y + r * KB_CELL_H;
        bool sel = (r == _row && c == _col);
        if (sel) {
          display.setColor(UIColor::primary_txt);
          display.fillRect(x, y - 1, KB_CELL_W, KB_CELL_H);
          display.setColor(UIColor::window_bkg);
        } else {
          display.setColor(UIColor::primary_txt);
        }
        ch[0] = row[c];
        display.setCursor(x + 3, y);
        display.print(ch);
      }
    }

    // special keys row
    const char* labels[KB_SPECIALS] = { shiftLabel(), "spc", "del", "OK" };
    int sw = display.width() / KB_SPECIALS;
    int y = KB_GRID_Y + KB_CHAR_ROWS * KB_CELL_H + 1;
    for (int k = 0; k < KB_SPECIALS; k++) {
      int x = k * sw;
      bool sel = (_row == KB_CHAR_ROWS && _col == k);
      display.setColor(UIColor::primary_txt);
      if (sel) {
        display.fillRect(x + 1, y - 1, sw - 2, KB_CELL_H);
        display.setColor(UIColor::window_bkg);
      } else {
        display.drawRect(x + 1, y - 1, sw - 2, KB_CELL_H);
      }
      display.drawTextCentered(x + sw / 2, y, labels[k]);
    }
    display.setColor(UIColor::primary_txt);
    return 5000;
  }

  bool handleInput(char c) override {
    bool on_specials = (_row == KB_CHAR_ROWS);
    int ncols = on_specials ? KB_SPECIALS : KB_COLS;
    switch (c) {
      case KEY_UP:
      case KEY_DOWN: {
        int new_row = (_row + (c == KEY_UP ? KB_ROWS - 1 : 1)) % KB_ROWS;
        bool to_specials = (new_row == KB_CHAR_ROWS);
        if (to_specials && !on_specials) _col = _col * KB_SPECIALS / KB_COLS;
        else if (!to_specials && on_specials) _col = _col * KB_COLS / KB_SPECIALS + 1;
        _row = new_row;
        return true;
      }
      case KEY_LEFT:
        _col = (_col + ncols - 1) % ncols;
        return true;
      case KEY_RIGHT:
        _col = (_col + 1) % ncols;
        return true;
      case KEY_ENTER:
        if (on_specials) pressSpecial(_col);
        else typeChar(kb_layers[_layer][_row][_col]);
        return true;
      case KEY_CONTEXT_MENU:
        finish(true);
        return true;
      case KEY_CANCEL:
        finish(false);
        return true;
    }
    return false;
  }
};
