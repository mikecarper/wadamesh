// SPDX-License-Identifier: GPL-3.0-or-later
#include "LineGame.h"

namespace gamesui {
static linegame::Board board;
static lv_obj_t *root, *status, *grid;
static const char* gridMap[6 * 8 + 1];
static const char* title() { return board.gravity ? "Four in a row" : "Tic-tac-toe"; }
static void close() {
  appPageEnd(close);
  if (root) appPageDeleteRootAsync(root);
  root = grid = status = nullptr;
}
static void draw() {
  size_t i = 0;
  for (int r = 0; r < board.rows(); ++r) {
    if (r) gridMap[i++] = "\n";
    for (int c = 0; c < board.cols(); ++c)
      gridMap[i++] = board.cells[r][c] == 1 ? "X" : board.cells[r][c] == 2 ? "O" : ".";
  }
  gridMap[i] = "";
  lv_btnmatrix_set_map(grid, gridMap);
  if (board.winner) lv_label_set_text_fmt(status, "%c wins", board.winner == 1 ? 'X' : 'O');
  else if (board.done()) lv_label_set_text(status, "Draw");
  else lv_label_set_text_fmt(status, "%c to play", board.turn == 1 ? 'X' : 'O');
}
static void start(bool gravity) {
  teamui::sheetClose();
  board = {}; board.gravity = gravity;
  root = appPageCreateRoot(COLOR_BG);
  appPageBegin(title(), close);
  status = lv_label_create(root);
  lv_obj_align(status, LV_ALIGN_TOP_LEFT, 5, 7);
  lv_obj_set_style_text_color(status, lv_color_hex(COLOR_TEXT), 0);
  lv_obj_t* reset = lv_btn_create(root);
  lv_obj_set_size(reset, 84, 28);
  lv_obj_align(reset, LV_ALIGN_TOP_RIGHT, -3, 2);
  lv_label_set_text(lv_label_create(reset), "New game");
  lv_obj_add_event_cb(reset, [](lv_event_t*) { bool g = board.gravity; board = {}; board.gravity = g; draw(); }, LV_EVENT_CLICKED, nullptr);
  grid = lv_btnmatrix_create(root);
  lv_obj_set_size(grid, LV_PCT(98), appPageContentH() - 39);
  lv_obj_align(grid, LV_ALIGN_BOTTOM_MID, 0, -2);
  lv_obj_add_event_cb(grid, [](lv_event_t*) {
    unsigned id = lv_btnmatrix_get_selected_btn(grid);
    if (id < unsigned(board.rows() * board.cols()) && board.play(id / board.cols(), id % board.cols())) draw();
  }, LV_EVENT_VALUE_CHANGED, nullptr);
  draw();
}
static void open() {
  lv_obj_t* card = teamui::sheetOpen("Games", "Artillery supports mesh opponents. Share one radio for the board games.");
  teamui::sheetBtn(card, "Artillery", [](lv_event_t*) { teamui::sheetClose(); ArtilleryGame::launch(); });
  teamui::sheetBtn(card, "Four in a row", [](lv_event_t*) { start(true); });
  teamui::sheetBtn(card, "Tic-tac-toe", [](lv_event_t*) { start(false); });
  teamui::sheetBtn(card, "Close", [](lv_event_t*) { teamui::sheetClose(); });
}
}
