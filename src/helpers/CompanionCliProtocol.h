#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace CompanionCliProtocol {

static constexpr uint8_t CMD_RUN_CLI_COMMAND = 66;
static constexpr uint8_t RESP_CODE_CLI_REPLY = 29;

struct ReplyCapture {
  char* data;
  size_t capacity;
  size_t length;
  size_t prefix_length;
  bool overflow;
};

inline bool containsNul(const uint8_t* data, size_t length) {
  return data && memchr(data, 0, length) != nullptr;
}

inline const char* parseRequest(const char* request, char prefix[4]) {
  prefix[0] = '\0';
  if (!request) return "";

  while (*request == ' ' || *request == '\t') request++;
  if (strlen(request) > 3 && request[2] == '|') {
    memcpy(prefix, request, 3);
    prefix[3] = '\0';
    request += 3;
    while (*request == ' ' || *request == '\t') request++;
  }
  return request;
}

inline void beginReply(ReplyCapture& capture, char* storage, size_t capacity,
                       const char prefix[4]) {
  capture.data = storage;
  capture.capacity = capacity;
  capture.length = 0;
  capture.prefix_length = 0;
  capture.overflow = false;

  if (prefix && prefix[0] != '\0') {
    if (capacity < 3) {
      capture.overflow = true;
    } else {
      memcpy(storage, prefix, 3);
      capture.length = 3;
      capture.prefix_length = 3;
    }
  }
  storage[capture.length] = '\0';
}

inline void appendReply(ReplyCapture& capture, const char* text) {
  if (!text || text[0] == '\0' || capture.overflow) return;

  const size_t text_length = strlen(text);
  const size_t separator_length = capture.length > capture.prefix_length ? 1 : 0;
  if (separator_length + text_length > capture.capacity - capture.length) {
    capture.overflow = true;
    return;
  }

  if (separator_length) capture.data[capture.length++] = '\n';
  memcpy(&capture.data[capture.length], text, text_length);
  capture.length += text_length;
  capture.data[capture.length] = '\0';
}

inline size_t finishReply(ReplyCapture& capture) {
  const char* fallback = capture.overflow
      ? "Error: CLI reply too long"
      : "Unknown command";
  if (capture.overflow || capture.length == capture.prefix_length) {
    capture.length = capture.prefix_length;
    capture.overflow = false;
    capture.data[capture.length] = '\0';
    appendReply(capture, fallback);
  }
  return capture.length;
}

inline size_t encodeReplyFrame(uint8_t* frame, size_t frame_capacity,
                               const ReplyCapture& capture) {
  if (!frame || !capture.data || frame_capacity == 0 ||
      capture.length > frame_capacity - 1) {
    return 0;
  }
  frame[0] = RESP_CODE_CLI_REPLY;
  memcpy(&frame[1], capture.data, capture.length);
  return 1 + capture.length;
}

}  // namespace CompanionCliProtocol
