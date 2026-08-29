#include <assert.h>
#include <string.h>

#include "helpers/CompanionCliProtocol.h"

int main() {
  using namespace CompanionCliProtocol;

  static_assert(CMD_RUN_CLI_COMMAND == 66, "local CLI command code changed");
  static_assert(RESP_CODE_CLI_REPLY == 29, "local CLI reply code changed");

  const uint8_t valid[] = {'g', 'e', 't'};
  const uint8_t invalid[] = {'g', 0, 't'};
  assert(!containsNul(valid, sizeof(valid)));
  assert(containsNul(invalid, sizeof(invalid)));

  char prefix[4];
  const char* command = parseRequest("  A7|  get radio.rxgain", prefix);
  assert(strcmp(prefix, "A7|") == 0);
  assert(strcmp(command, "get radio.rxgain") == 0);

  command = parseRequest("status", prefix);
  assert(prefix[0] == '\0');
  assert(strcmp(command, "status") == 0);

  command = parseRequest("A7| ", prefix);
  assert(strcmp(prefix, "A7|") == 0);
  assert(command[0] == '\0');

  char storage[64];
  ReplyCapture capture;
  beginReply(capture, storage, sizeof(storage) - 1, "A7|");
  appendReply(capture, "first");
  appendReply(capture, "second");
  assert(finishReply(capture) == strlen("A7|first\nsecond"));
  assert(strcmp(storage, "A7|first\nsecond") == 0);
  uint8_t frame[64];
  const size_t frame_length = encodeReplyFrame(frame, sizeof(frame), capture);
  assert(frame_length == 1 + strlen("A7|first\nsecond"));
  assert(frame[0] == RESP_CODE_CLI_REPLY);
  assert(memcmp(&frame[1], "A7|first\nsecond", frame_length - 1) == 0);
  assert(encodeReplyFrame(frame, frame_length - 1, capture) == 0);

  beginReply(capture, storage, 32, "A7|");
  appendReply(capture, "This reply is intentionally too long");
  assert(finishReply(capture) == strlen("A7|Error: CLI reply too long"));
  assert(strcmp(storage, "A7|Error: CLI reply too long") == 0);

  beginReply(capture, storage, sizeof(storage) - 1, "");
  assert(finishReply(capture) == strlen("Unknown command"));
  assert(strcmp(storage, "Unknown command") == 0);
  return 0;
}
