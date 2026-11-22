// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdint.h>

namespace linegame {
struct Board {
  uint8_t cells[6][7]{};
  uint8_t turn = 1, winner = 0, moves = 0;
  bool gravity = false;

  int rows() const { return gravity ? 6 : 3; }
  int cols() const { return gravity ? 7 : 3; }
  bool done() const { return winner || moves == rows() * cols(); }
  int streak(int r, int c, int dr, int dc) const {
    int n = 0;
    for (r += dr, c += dc; r >= 0 && r < rows() && c >= 0 && c < cols() && cells[r][c] == turn; r += dr, c += dc) ++n;
    return n;
  }
  bool play(int r, int c) {
    if (done() || c < 0 || c >= cols() || r < 0 || r >= rows()) return false;
    if (gravity) for (r = rows() - 1; r > 0 && cells[r][c]; --r) {}
    if (cells[r][c]) return false;
    cells[r][c] = turn;
    ++moves;
    const int need = gravity ? 4 : 3;
    const int dirs[][2] = {{0,1},{1,0},{1,1},{1,-1}};
    for (const auto& d : dirs)
      if (1 + streak(r,c,d[0],d[1]) + streak(r,c,-d[0],-d[1]) >= need) winner = turn;
    if (!done()) turn = 3 - turn;
    return true;
  }
};
}
