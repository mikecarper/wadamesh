#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "ui-touch/gnuboy/gnuboy.h"
#include "ui-touch/gnuboy/hw.h"

static bool handle_transfer;
static uint8_t seen_outgoing;
static bool seen_internal;

static bool serial_start(uint8_t outgoing, bool internal_clock) {
  seen_outgoing = outgoing;
  seen_internal = internal_clock;
  return handle_transfer;
}

int main(void) {
  static uint8_t rom[0x8000];
  rom[0x0100] = 0x3E;  // LD A,0xA5
  rom[0x0101] = 0xA5;
  rom[0x0102] = 0xE0;  // LDH (0x01),A
  rom[0x0103] = 0x01;
  rom[0x0104] = 0x3E;  // LD A,0x81
  rom[0x0105] = 0x81;
  rom[0x0106] = 0xE0;  // LDH (0x02),A
  rom[0x0107] = 0x02;
  rom[0x0147] = 0x00;  // no MBC
  rom[0x0148] = 0x00;  // two 16 KiB banks
  rom[0x0149] = 0x00;  // no cartridge RAM

  assert(gnuboy_init(22050, GB_AUDIO_MONO_S16, GB_PIXEL_565_LE, 0, 0) == 0);
  assert(gnuboy_load_rom(rom, sizeof(rom)) == 0);
  gnuboy_reset(true);
  gnuboy_set_serial_callback(serial_start);

  handle_transfer = true;
  gb_hw_write(0xFF01, 0xA5);
  gb_hw_write(0xFF02, 0x81);
  assert(seen_outgoing == 0xA5);
  assert(seen_internal);
  assert(gnuboy_serial_waiting());
  assert(GB.serial == 0);
  assert((R_SC & 0x80) != 0);

  gnuboy_serial_complete(0x5A);
  assert(!gnuboy_serial_waiting());
  assert(R_SB == 0x5A);
  assert((R_SC & 0x80) == 0);
  assert((R_IF & IF_SERIAL) != 0);

  handle_transfer = false;
  gb_hw_write(0xFF01, 0x3C);
  gb_hw_write(0xFF02, 0x81);
  assert(!gnuboy_serial_waiting());
  assert(GB.serial == 1952);

  handle_transfer = false;
  gb_hw_write(0xFF02, 0x80);
  assert(!seen_internal);
  assert(!gnuboy_serial_waiting());
  assert((R_SC & 0x80) != 0);
  handle_transfer = true;
  assert(gnuboy_serial_rearm());
  assert(gnuboy_serial_waiting());
  gnuboy_serial_cancel();
  assert(!gnuboy_serial_waiting());
  assert((R_SC & 0x80) == 0);

  gnuboy_reset(true);
  handle_transfer = true;
  assert(!gnuboy_run(false));
  assert(gnuboy_serial_waiting());
  assert(seen_outgoing == 0xA5);
  assert(seen_internal);
  gnuboy_serial_complete(0xC3);
  assert(gnuboy_run(false));

  gnuboy_deinit();
  return 0;
}
