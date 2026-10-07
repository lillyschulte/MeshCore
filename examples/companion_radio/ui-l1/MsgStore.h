#pragma once

#include <Arduino.h>
#include <string.h>
#include <helpers/TxtDataHelpers.h>

#ifndef UI_MSG_STORE_SIZE
  #define UI_MSG_STORE_SIZE  64
#endif
#define UI_MSG_TEXT_LEN     160
#define UI_MSG_SENDER_LEN    24

#define CHAT_KIND_NONE     0
#define CHAT_KIND_DM       1
#define CHAT_KIND_CHANNEL  2

// identifies a conversation: a contact (6-byte pub key prefix) or a channel (index)
struct ChatKey {
  uint8_t kind;
  uint8_t id[6];

  static ChatKey dm(const uint8_t* pub_key) {
    ChatKey k;
    k.kind = CHAT_KIND_DM;
    memcpy(k.id, pub_key, sizeof(k.id));
    return k;
  }
  static ChatKey channel(uint8_t idx) {
    ChatKey k;
    k.kind = CHAT_KIND_CHANNEL;
    memset(k.id, 0, sizeof(k.id));
    k.id[0] = idx;
    return k;
  }
  bool operator==(const ChatKey& o) const { return kind == o.kind && memcmp(id, o.id, sizeof(id)) == 0; }
  bool isChannel() const { return kind == CHAT_KIND_CHANNEL; }
  uint8_t channelIdx() const { return id[0]; }
};

#define MSG_FLAG_OUT        0x01
#define MSG_FLAG_UNREAD     0x02
#define MSG_FLAG_PENDING    0x04
#define MSG_FLAG_DELIVERED  0x08
#define MSG_FLAG_FAILED     0x10

struct UIMsg {
  ChatKey  key;
  uint8_t  flags;
  uint32_t timestamp;       // by OUR clock
  uint32_t ack;             // expected ACK (outgoing DMs)
  unsigned long deadline;   // millis() after which a PENDING message is FAILED
  char     sender[UI_MSG_SENDER_LEN];
  char     text[UI_MSG_TEXT_LEN + 1];
};

// RAM-only ring buffer of recent messages, oldest entries are evicted first
class MsgStore {
  UIMsg _msgs[UI_MSG_STORE_SIZE];
  int _next;    // next slot to write
  int _count;

public:
  MsgStore() : _next(0), _count(0) { }

  int count() const { return _count; }

  // i = 0 is the oldest message
  UIMsg* get(int i) {
    if (i < 0 || i >= _count) return NULL;
    int idx = (_next - _count + i + UI_MSG_STORE_SIZE) % UI_MSG_STORE_SIZE;
    return &_msgs[idx];
  }

  UIMsg* add(const ChatKey& key, uint8_t flags, uint32_t timestamp, const char* sender, const char* text) {
    UIMsg* m = &_msgs[_next];
    _next = (_next + 1) % UI_MSG_STORE_SIZE;
    if (_count < UI_MSG_STORE_SIZE) _count++;

    memset(m, 0, sizeof(*m));
    m->key = key;
    m->flags = flags;
    m->timestamp = timestamp;
    StrHelper::strncpy(m->sender, sender ? sender : "", sizeof(m->sender));
    StrHelper::strncpy(m->text, text ? text : "", sizeof(m->text));
    return m;
  }

  int unreadCount() const {
    int n = 0;
    for (int i = 0; i < _count; i++) {
      if (_msgs[i].flags & MSG_FLAG_UNREAD) n++;
    }
    return n;
  }

  int unreadCount(const ChatKey& key) {
    int n = 0;
    for (int i = 0; i < _count; i++) {
      UIMsg* m = get(i);
      if (m->key == key && (m->flags & MSG_FLAG_UNREAD)) n++;
    }
    return n;
  }

  void markRead(const ChatKey& key) {
    for (int i = 0; i < _count; i++) {
      UIMsg* m = get(i);
      if (m->key == key) m->flags &= ~MSG_FLAG_UNREAD;
    }
  }

  UIMsg* findPendingByAck(uint32_t ack) {
    if (ack == 0) return NULL;
    for (int i = 0; i < _count; i++) {
      UIMsg* m = get(i);
      if ((m->flags & MSG_FLAG_OUT) && m->ack == ack) return m;
    }
    return NULL;
  }

  UIMsg* latestFor(const ChatKey& key) {
    for (int i = _count - 1; i >= 0; i--) {
      UIMsg* m = get(i);
      if (m->key == key) return m;
    }
    return NULL;
  }

  // distinct conversations, newest activity first; returns number written to dest
  int getThreads(ChatKey dest[], int max_num) {
    int n = 0;
    for (int i = _count - 1; i >= 0 && n < max_num; i--) {
      UIMsg* m = get(i);
      bool seen = false;
      for (int j = 0; j < n; j++) {
        if (dest[j] == m->key) { seen = true; break; }
      }
      if (!seen) dest[n++] = m->key;
    }
    return n;
  }

  // mark PENDING messages whose ACK never arrived as FAILED; returns true if any changed
  bool checkTimeouts(unsigned long now) {
    bool changed = false;
    for (int i = 0; i < _count; i++) {
      UIMsg* m = &_msgs[i];
      if ((m->flags & MSG_FLAG_PENDING) && (long)(now - m->deadline) >= 0) {
        m->flags = (m->flags & ~MSG_FLAG_PENDING) | MSG_FLAG_FAILED;
        changed = true;
      }
    }
    return changed;
  }
};
