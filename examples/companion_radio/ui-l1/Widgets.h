#pragma once

#include "UITask.h"

// layout for the 128x64 OLED, text size 1 = 6x8 px glyphs
#define UI_TITLE_H      9
#define UI_ROW_H        9
#define UI_LIST_TOP    10
#define UI_LIST_ROWS    6

// title bar: inverted strip with title on the left and the clock on the right
static void drawTitleBar(DisplayDriver& display, UITask* task, const char* title) {
  display.setTextSize(1);
  display.setColor(UIColor::primary_txt);
  display.fillRect(0, 0, display.width(), UI_TITLE_H);
  display.setColor(UIColor::window_bkg);

  char clock[12] = "";
  if (task->isClockSet()) task->formatClock(task->getLocalTime(), clock, false);
  int clock_w = clock[0] ? display.getTextWidth(clock) : 0;
  if (clock[0]) {
    display.setCursor(display.width() - clock_w - 1, 1);
    display.print(clock);
  }

  char filtered[48];
  display.translateUTF8ToBlocks(filtered, title, sizeof(filtered));
  display.drawTextEllipsized(1, 1, display.width() - clock_w - 5, filtered);
  display.setColor(UIColor::primary_txt);
}

// draw one list row, inverted when selected
static void drawRow(DisplayDriver& display, int y, bool selected, const char* label, const char* value, int right_margin) {
  int w = display.width() - right_margin;
  if (selected) {
    display.setColor(UIColor::primary_txt);
    display.fillRect(0, y, w, UI_ROW_H);
    display.setColor(UIColor::window_bkg);
  } else {
    display.setColor(UIColor::primary_txt);
  }
  int value_w = 0;
  if (value && value[0]) {
    value_w = display.getTextWidth(value);
    display.setCursor(w - value_w - 1, y + 1);
    display.print(value);
    value_w += 4;
  }
  char filtered[48];
  display.translateUTF8ToBlocks(filtered, label, sizeof(filtered));
  display.drawTextEllipsized(2, y + 1, w - value_w - 2, filtered);
  display.setColor(UIColor::primary_txt);
}

static void drawScrollBar(DisplayDriver& display, int top, int visible, int total) {
  if (total <= visible) return;
  int track_h = display.height() - UI_LIST_TOP;
  int bar_h = track_h * visible / total;
  if (bar_h < 4) bar_h = 4;
  int bar_y = UI_LIST_TOP + (track_h - bar_h) * top / (total - visible);
  display.setColor(UIColor::primary_txt);
  display.fillRect(display.width() - 2, bar_y, 2, bar_h);
}

// word-wrap 'text' into lines of at most 'cols' chars; fn(const char* start, int len) is called per line
template <typename F>
static int wrapText(const char* text, int cols, F fn) {
  int lines = 0;
  const char* p = text;
  while (*p == ' ') p++;
  if (*p == 0) { fn(p, 0); return 1; }
  while (*p) {
    int len = strlen(p);
    if (len <= cols) {
      fn(p, len);
      lines++;
      break;
    }
    int brk = -1;
    for (int i = cols; i > 0; i--) {
      if (p[i] == ' ') { brk = i; break; }
    }
    if (brk <= 0) {   // no space to break at, hard wrap
      fn(p, cols);
      p += cols;
    } else {
      fn(p, brk);
      p += brk + 1;
    }
    lines++;
    while (*p == ' ') p++;
  }
  return lines;
}

static void formatAge(char* dest, uint32_t secs) {
  if (secs < 60) {
    sprintf(dest, "%ds", (int) secs);
  } else if (secs < 60*60) {
    sprintf(dest, "%dm", (int) (secs / 60));
  } else if (secs < 48*60*60) {
    sprintf(dest, "%dh", (int) (secs / (60*60)));
  } else {
    sprintf(dest, "%dd", (int) (secs / (24*60*60)));
  }
}

// generic scrolling, selectable list
class ListScreen : public UIScreen {
protected:
  UITask* _task;
  int _sel, _top;

  virtual const char* getTitle() = 0;
  virtual int  getCount() = 0;
  virtual void getItem(int i, char* label, int label_size, char* value, int value_size) = 0;
  virtual void onEnter(int i) { }
  virtual bool onLeftRight(int i, int dir) { return false; }   // return true if handled (eg. change a value)
  virtual void onContextMenu(int i) { }
  virtual const char* getEmptyText() { return "(empty)"; }

public:
  ListScreen(UITask* task) : _task(task), _sel(0), _top(0) { }

  void reset() { _sel = _top = 0; }

  int render(DisplayDriver& display) override {
    drawTitleBar(display, _task, getTitle());

    int count = getCount();
    if (count == 0) {
      display.setColor(UIColor::secondary_txt);
      display.drawTextCentered(display.width() / 2, 30, getEmptyText());
      return 2000;
    }
    if (_sel >= count) _sel = count - 1;
    if (_sel < _top) _top = _sel;
    if (_sel >= _top + UI_LIST_ROWS) _top = _sel - UI_LIST_ROWS + 1;
    if (_top > count - UI_LIST_ROWS) _top = count > UI_LIST_ROWS ? count - UI_LIST_ROWS : 0;

    int right_margin = count > UI_LIST_ROWS ? 3 : 0;
    char label[48], value[24];
    for (int r = 0; r < UI_LIST_ROWS && _top + r < count; r++) {
      label[0] = value[0] = 0;
      getItem(_top + r, label, sizeof(label), value, sizeof(value));
      drawRow(display, UI_LIST_TOP + r * UI_ROW_H, _top + r == _sel, label, value, right_margin);
    }
    drawScrollBar(display, _top, UI_LIST_ROWS, count);
    return 2000;
  }

  bool handleInput(char c) override {
    int count = getCount();
    if (c == KEY_UP || c == KEY_PREV) {
      if (count > 0) _sel = (_sel + count - 1) % count;
      return true;
    }
    if (c == KEY_DOWN || c == KEY_NEXT) {
      if (count > 0) _sel = (_sel + 1) % count;
      return true;
    }
    if (c == KEY_LEFT) {
      if (count > 0 && onLeftRight(_sel, -1)) return true;
      _task->pop();
      return true;
    }
    if (c == KEY_CANCEL) {
      _task->pop();
      return true;
    }
    if (c == KEY_RIGHT) {
      if (count > 0 && !onLeftRight(_sel, 1)) onEnter(_sel);
      return true;
    }
    if (c == KEY_ENTER) {
      if (count > 0) onEnter(_sel);
      return true;
    }
    if (c == KEY_CONTEXT_MENU) {
      if (count > 0) onContextMenu(_sel);
      return true;
    }
    return false;
  }
};
