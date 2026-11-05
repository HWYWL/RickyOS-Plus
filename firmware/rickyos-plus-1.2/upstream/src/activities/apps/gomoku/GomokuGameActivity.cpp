#include "GomokuGameActivity.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "GomokuAI.h"
#include "GomokuArt.h"
#include "GomokuLayout.h"
#include "GomokuMenuActivity.h"
#include "activities/apps/GameUi.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

namespace ds = ricky::ds;
namespace fui = freeink::ui;

const char* modeLabel(GomokuMode m) {
  return (m == GomokuMode::VsAi) ? tr(STR_GOMOKU_MODE_AI) : tr(STR_GOMOKU_MODE_2P);
}

}  // namespace

GomokuGameActivity::GomokuGameActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, GomokuMode mode,
                                       uint8_t boardSize, bool resume, GomokuAiLevel aiLevel)
    : Activity("Gomoku", renderer, mappedInput), mode(mode), aiLevel(aiLevel), resumeRequested(resume) {
  board.clear((boardSize == 9) ? 9 : 15);
  cursorR = static_cast<uint8_t>((board.boardSize - 1) / 2);
  cursorC = cursorR;
}

void GomokuGameActivity::onEnter() {
  Activity::onEnter();
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  state = State::Playing;
  elapsedMs = 0;
  lastTickMs = millis();
  saveDebouncer.clear();
  statsRecorded = false;
  resignedFlag = false;
  resignWinner = GomokuBoard::Stone::Empty;

  if (resumeRequested) {
    GomokuSaveSlot slot;
    if (GomokuStore::load(slot)) {
      board = slot.board;
      mode = slot.mode;
      aiLevel = slot.aiLevel;
      cursorR = (slot.cursorR < board.boardSize) ? slot.cursorR : static_cast<uint8_t>((board.boardSize - 1) / 2);
      cursorC = (slot.cursorC < board.boardSize) ? slot.cursorC : cursorR;
      elapsedMs = static_cast<uint32_t>(slot.elapsedSec) * 1000u;
      // Resumed game already counted via recordStart on its first start; do not double count.
      // If the saved board has a winner already, jump straight to GameOver (rare but possible).
      if (board.winner != GomokuBoard::Stone::Empty || board.isFull()) {
        state = State::GameOver;
        statsRecorded = true;  // already recorded on the original session.
      }
    } else {
      LOG_ERR("GMK", "Resume save unreadable; clearing and starting fresh");
      GomokuStore::clear();
      resumeRequested = false;
      GomokuStore::recordStart(board.boardSize);
    }
  } else {
    GomokuStore::recordStart(board.boardSize);
  }

  // If we resumed mid-game on AI's turn, queue a thinking pass so the AI
  // plays its move once we render the first frame.
  if (state == State::Playing && aiToMove()) {
    aiThinkingArmed = true;
    aiThinkingShown = false;
  }

  requestUpdate();
}

void GomokuGameActivity::onExit() {
  flushSave();
  Activity::onExit();
}

void GomokuGameActivity::loop() {
  if (state == State::Playing) {
    const uint32_t now = millis();
    if (now > lastTickMs) {
      elapsedMs += now - lastTickMs;
    }
    lastTickMs = now;

    if (saveDebouncer.consumeIfDue(now)) {
      flushSave();
    }

    // Two-stage AI move: armed → render "Thinking…" once → search → place.
    // Forcing the render before the (possibly multi-second) search keeps the
    // e-ink screen responsive and gives the player feedback that input is
    // being handled.
    if (aiThinkingArmed) {
      if (!aiThinkingShown) {
        aiThinkingShown = true;
        requestUpdateAndWait();  // block until "Thinking…" is on screen
        return;
      }
      runAiTurn();
      aiThinkingArmed = false;
      aiThinkingShown = false;
      requestUpdate();
      return;
    }

    handleInputPlaying();
  } else if (state == State::GameMenu) {
    handleInputGameMenu();
  } else if (state == State::GameOver) {
    handleInputGameOver();
  }
}

// ---------- Geometry ----------

// Under the system header; the player cards sit just above the action bar.
int GomokuGameActivity::boardAreaTop() const {
  const auto& m = UITheme::getInstance().getMetrics();
  return m.topPadding + m.headerHeight + ds::space::k2;
}
int GomokuGameActivity::playersHeight() const {
  return renderer.getLineHeight(RickyDesignUi::BODY) + renderer.getLineHeight(RickyDesignUi::CAPTION) +
         ds::space::kTextStackGap + 2 * ds::space::k4;
}
int GomokuGameActivity::playersTop() const { return actionBarY() - ds::space::k3 - playersHeight(); }

int GomokuGameActivity::boardPitch() const {
  // Keep button-only boards' old tuning; fit every touch board between the header and
  // the player cards, inside the system page margins.
  if (mappedInput.hasTouch()) {
    const int side = ds::space::kPageMargin;
    const auto layout = GomokuLayout::fit(side, boardAreaTop(), renderer.getScreenWidth() - 2 * side,
                                          playersTop() - ds::space::k3 - boardAreaTop(), board.boardSize);
    return std::max(1, layout.pitch);
  }
  return (board.boardSize == 15) ? 30 : 50;
}
int GomokuGameActivity::boardOriginX() const {
  return (renderer.getScreenWidth() - (board.boardSize - 1) * boardPitch()) / 2;
}
int GomokuGameActivity::boardOriginY() const {
  if (mappedInput.hasTouch()) {
    const int used = (board.boardSize - 1) * boardPitch();
    const int available = playersTop() - ds::space::k3 - boardAreaTop();
    return boardAreaTop() + std::max(0, (available - used) / 2);
  }
  return boardAreaTop() + boardPitch() / 2;
}
int GomokuGameActivity::stoneRadius() const {
  if (mappedInput.hasTouch()) return std::max(1, boardPitch() * 2 / 5);  // matches gomokuart and the skill board
  return (board.boardSize == 15) ? 12 : 20;
}

void GomokuGameActivity::intersectionXY(uint8_t r, uint8_t c, int* x, int* y) const {
  *x = boardOriginX() + static_cast<int>(c) * boardPitch();
  *y = boardOriginY() + static_cast<int>(r) * boardPitch();
}

// The board's own rectangle: the grid plus half a pitch of slop, which is
// exactly the intersection hit tolerance. Touch inside it aims the cursor.
Rect GomokuGameActivity::boardTouchRect() const {
  const int pitch = boardPitch();
  const int n = board.boardSize;
  const int ox = boardOriginX();
  const int oy = boardOriginY();
  const int len = (n - 1) * pitch;
  const int pad = pitch / 2;
  return Rect{ox - pad, oy - pad, len + 2 * pad, len + 2 * pad};
}

// Bottom action bar geometry, scaled from the panel size rather than pinned to
// absolute pixels: this firmware ships several targets with different panels, so
// the bar is measured UP FROM THE SCREEN BOTTOM and sized as a fraction of the
// panel height, with a floor so it stays a comfortable touch target.
int GomokuGameActivity::actionBarH() const {
  return std::max(ACTION_BAR_MIN_H, renderer.getScreenHeight() * ACTION_BAR_H_FRAC / 100);
}

int GomokuGameActivity::actionBarY() const {
  const int h = renderer.getScreenHeight();
  return h - actionBarH() - std::max(4, h * ACTION_BAR_BOTTOM_FRAC / 100);
}

Rect GomokuGameActivity::actionBarRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int side = std::max(8, metrics.contentSidePadding);
  return Rect{side, actionBarY(), renderer.getScreenWidth() - 2 * side, actionBarH()};
}

// ---------- Input ----------

void GomokuGameActivity::handleInputPlaying() {
  int touchX = 0;
  int touchY = 0;
  // Touch never places a stone: touching the board only aims the cursor. The
  // 15x15 grid has 30 px cells, so a finger landing "on" an intersection can
  // still be aiming at its neighbour; placing on touch turns a mis-aim into an
  // irreversible move. The bottom action bar commits the stone at the
  // highlighted cursor, so aiming and committing are separate gestures.
  const Rect boardRect = boardTouchRect();
  const Rect placeButton = actionBarRect();
  const auto inRect = [](const Rect& r, const int x, const int y) {
    return r.width > 0 && r.height > 0 && x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
  };
  const auto aimAt = [&](const int x, const int y) {
    int row = 0;
    int column = 0;
    if (!gameIntersectionFromPoint(boardOriginX(), boardOriginY(), boardPitch(), board.boardSize, board.boardSize, x, y,
                                   row, column)) {
      return false;
    }
    if (cursorR == static_cast<uint8_t>(row) && cursorC == static_cast<uint8_t>(column)) return false;
    cursorR = static_cast<uint8_t>(row);
    cursorC = static_cast<uint8_t>(column);
    requestUpdate();
    return true;
  };

  // The action bar is checked first: it sits below the board, but checking it up
  // front keeps the two regions from ever fighting over an edge pixel.
  if (mappedInput.wasScreenTapped(touchX, touchY) && inRect(placeButton, touchX, touchY)) {
    doPlace();
    requestUpdate();
    return;
  }

  if (mappedInput.wasScreenTouchDown(touchX, touchY) && inRect(boardRect, touchX, touchY)) {
    aimAt(touchX, touchY);
  }

  if (mappedInput.wasScreenTapped(touchX, touchY) && inRect(boardRect, touchX, touchY)) {
    aimAt(touchX, touchY);
    return;  // aim only — the stone is committed by the action bar or Confirm
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    moveCursor(-1, 0);
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    moveCursor(1, 0);
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    moveCursor(0, -1);
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    moveCursor(0, 1);
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    doPlace();
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    enterGameMenu();
    requestUpdate();
  }
}

void GomokuGameActivity::handleInputGameMenu() {
  gameMenu.handleInput(mappedInput, [this] { requestUpdate(); });
  if (!gameMenu.isActive() && state == State::GameMenu) resumeFromMenu();
}

void GomokuGameActivity::handleInputGameOver() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect again = gameTouchActionRect(renderer.getScreenWidth(), renderer.getScreenHeight(),
                                         metrics.contentSidePadding, metrics.menuSpacing, metrics.menuRowHeight, 0, 2);
  const Rect home = gameTouchActionRect(renderer.getScreenWidth(), renderer.getScreenHeight(),
                                        metrics.contentSidePadding, metrics.menuSpacing, metrics.menuRowHeight, 1, 2);
  if (mappedInput.wasTapInRect(again.x, again.y, again.width, again.height) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activityManager.replaceActivityWith<GomokuGameActivity>(mode, board.boardSize, false, aiLevel);
    return;
  }
  if (mappedInput.wasTapInRect(home.x, home.y, home.width, home.height) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.replaceActivityWith<GomokuMenuActivity>();
  }
}

void GomokuGameActivity::moveCursor(int dr, int dc) {
  const int n = board.boardSize;
  cursorR = static_cast<uint8_t>((cursorR + dr + n) % n);
  cursorC = static_cast<uint8_t>((cursorC + dc + n) % n);
}

void GomokuGameActivity::doPlace() {
  if (board.winner != GomokuBoard::Stone::Empty) return;
  if (!board.isEmpty(cursorR, cursorC)) return;  // can't place on occupied intersection
  if (!board.placeStone(cursorR, cursorC)) return;
  scheduleSave();
  if (board.winner != GomokuBoard::Stone::Empty || board.isFull()) {
    onGameOver();
    return;
  }
  if (aiToMove()) {
    aiThinkingArmed = true;
    aiThinkingShown = false;
  }
}

bool GomokuGameActivity::aiToMove() const {
  return mode == GomokuMode::VsAi && state == State::Playing && board.winner == GomokuBoard::Stone::Empty &&
         !board.isFull() && board.nextTurn() == kAiSide;
}

void GomokuGameActivity::runAiTurn() {
  uint8_t mv = GomokuAI::chooseMove(board, kAiSide, aiLevel);
  uint8_t r = board.rowOf(mv);
  uint8_t c = board.colOf(mv);
  if (!board.placeStone(r, c)) {
    // Should never happen — chooseMove only picks empty cells. If it does,
    // skipping the move would leave nextTurn() pointing at the AI's color,
    // and the player's next placement would be misattributed. Find any
    // empty intersection as a passive fallback so the turn still advances.
    LOG_ERR("GMK", "AI returned illegal move idx=%u; using fallback", static_cast<unsigned>(mv));
    bool placed = false;
    const uint16_t total = static_cast<uint16_t>(board.boardSize) * board.boardSize;
    for (uint16_t i = 0; i < total; i++) {
      if (board.cells[i] == static_cast<uint8_t>(GomokuBoard::Stone::Empty)) {
        r = board.rowOf(static_cast<uint8_t>(i));
        c = board.colOf(static_cast<uint8_t>(i));
        if (board.placeStone(r, c)) {
          placed = true;
          break;
        }
      }
    }
    if (!placed) return;  // no empty cell — board is full, draw will be detected below
  }
  // Move cursor to AI's stone so the next player input picks up nearby —
  // and the cursor box visibly reflects the new state.
  cursorR = r;
  cursorC = c;
  scheduleSave();
  if (board.winner != GomokuBoard::Stone::Empty || board.isFull()) {
    onGameOver();
  }
}

void GomokuGameActivity::onGameOver() {
  if (state == State::GameOver) return;
  state = State::GameOver;
  if (!statsRecorded) {
    const uint32_t totalSec = elapsedMs / 1000;
    const uint16_t cap = static_cast<uint16_t>(totalSec > 0xFFFF ? 0xFFFF : totalSec);
    if (resignedFlag) {
      GomokuStore::recordWin(board.boardSize, resignWinner, cap);
    } else if (board.winner != GomokuBoard::Stone::Empty) {
      GomokuStore::recordWin(board.boardSize, board.winner, cap);
    } else {
      // Board full without a 5-in-a-row.
      GomokuStore::recordDraw(board.boardSize);
    }
    GomokuStore::clear();
    saveDebouncer.clear();
    statsRecorded = true;
  }
}

void GomokuGameActivity::enterGameMenu() {
  state = State::GameMenu;
  const char* options[MENU_ITEM_COUNT] = {
      tr(STR_GAME_RESUME), tr(STR_GOMOKU_UNDO), tr(STR_GOMOKU_RESIGN), tr(STR_GAME_NEW_GAME), tr(STR_GAME_EXIT),
  };
  gameMenu.show(tr(STR_GAME_GAME_MENU), options, MENU_ITEM_COUNT, 0,
                [this](const int index) { runMenuItem(static_cast<uint8_t>(index)); });
}

void GomokuGameActivity::resumeFromMenu() {
  state = State::Playing;
  lastTickMs = millis();
}

void GomokuGameActivity::runMenuItem(uint8_t i) {
  switch (i) {
    case 0:  // Resume
      resumeFromMenu();
      return;
    case 1:  // Undo
      if (board.moveCount > 0) {
        board.undo();
        // In vs-AI mode, undoing once would just hand the turn back to the AI
        // which immediately replays — undo a second step so the player gets
        // their decision back. (Skip when the player just played the very
        // first move and the AI hasn't responded yet.)
        if (mode == GomokuMode::VsAi && board.moveCount > 0 && board.nextTurn() == kAiSide) {
          board.undo();
        }
        aiThinkingArmed = false;
        aiThinkingShown = false;
        scheduleSave();
      }
      resumeFromMenu();
      return;
    case 2: {  // Resign — current side to move resigns; the other side wins.
      const GomokuBoard::Stone resigner = board.nextTurn();
      resignedFlag = true;
      resignWinner = (resigner == GomokuBoard::Stone::Black) ? GomokuBoard::Stone::White : GomokuBoard::Stone::Black;
      board.winner = resignWinner;  // for win-screen display
      board.winLineStart = GomokuBoard::INVALID_IDX;
      board.winLineEnd = GomokuBoard::INVALID_IDX;
      onGameOver();
      return;
    }
    case 3: {  // New Game (same mode + size + difficulty)
      const auto m = mode;
      const uint8_t bs = board.boardSize;
      const auto lv = aiLevel;
      GomokuStore::clear();
      activityManager.replaceActivityWith<GomokuGameActivity>(m, bs, false, lv);
      return;
    }
    case 4:  // Exit to menu
      flushSave();
      activityManager.replaceActivityWith<GomokuMenuActivity>();
      return;
  }
}

void GomokuGameActivity::scheduleSave() { saveDebouncer.schedule(millis()); }

void GomokuGameActivity::flushSave() {
  if (state != State::Playing && state != State::GameMenu) return;
  saveDebouncer.clear();
  GomokuSaveSlot slot;
  slot.board = board;
  slot.mode = mode;
  slot.aiLevel = aiLevel;
  slot.cursorR = cursorR;
  slot.cursorC = cursorC;
  slot.elapsedSec = static_cast<uint16_t>(elapsedMs / 1000);
  if (!GomokuStore::save(slot)) {
    LOG_ERR("GMK", "Failed to write save");
  }
}

// ---------- Render dispatch ----------

void GomokuGameActivity::render(RenderLock&&) {
  renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  renderer.clearScreen();

  switch (state) {
    case State::Playing:
      renderPlaying();
      break;
    case State::GameMenu:
      renderPlaying();
      if (gameMenu.processRender(renderer, mappedInput)) return;
      break;
    case State::GameOver:
      renderGameOver();
      break;
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void GomokuGameActivity::renderPlaying() {
  drawTitleBar();
  drawBoard();
  drawInfoPanel();
  drawModeLine();
  // Touch targets: the place button is the only way to commit a stone by touch,
  // since tapping the board just aims the cursor.
  if (mappedInput.hasTouch()) {
    GUI.drawActionButton(renderer, actionBarRect(), tr(STR_GOMOKU_PLACE), true);
  }
  drawFooter();
}

// ---------- Title bar ----------

void GomokuGameActivity::drawTitleBar() {
  // The system header: "← 五子棋" with the mode, move count and time as its subtitle.
  char timeStr[8];
  gameFormatElapsed(elapsedMs, timeStr, sizeof(timeStr));
  char moves[24];
  snprintf(moves, sizeof(moves), tr(STR_GOMOKU_MOVE_FMT), static_cast<unsigned>(board.moveCount));
  char subtitle[96];
  if (mode == GomokuMode::VsAi) {
    const char* level = aiLevel == GomokuAiLevel::Easy   ? tr(STR_GOMOKU_DIFF_EASY)
                        : aiLevel == GomokuAiLevel::Hard ? tr(STR_GOMOKU_DIFF_HARD)
                                                         : tr(STR_GOMOKU_DIFF_MEDIUM);
    snprintf(subtitle, sizeof(subtitle), "%s · %s · %s · %s", modeLabel(mode), level, moves, timeStr);
  } else {
    snprintf(subtitle, sizeof(subtitle), "%s · %s · %s", modeLabel(mode), moves, timeStr);
  }
  const auto& m = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, m.topPadding, renderer.getScreenWidth(), m.headerHeight}, tr(STR_GOMOKU_TITLE),
                 subtitle);
}

// ---------- Board ----------

void GomokuGameActivity::drawBoard() {
  const int n = board.boardSize;
  const int pitch = boardPitch();
  const int ox = boardOriginX();
  const int oy = boardOriginY();
  gomokuart::board(renderer, ox, oy, pitch, n);

  const int sr = stoneRadius();
  for (int r = 0; r < n; r++) {
    for (int c = 0; c < n; c++) {
      const auto v = board.at(static_cast<uint8_t>(r), static_cast<uint8_t>(c));
      if (v == GomokuBoard::Stone::Empty) continue;
      gomokuart::stone(renderer, ox + c * pitch, oy + r * pitch, sr, v == GomokuBoard::Stone::Black);
    }
  }
  if (board.moveCount > 0) {
    const uint8_t lastIdx = board.moveHistory[board.moveCount - 1];
    const uint8_t lr = board.rowOf(lastIdx);
    const uint8_t lc = board.colOf(lastIdx);
    gomokuart::lastMove(renderer, ox + lc * pitch, oy + lr * pitch, sr, board.at(lr, lc) == GomokuBoard::Stone::Black);
  }
  if (state == State::Playing && board.winner == GomokuBoard::Stone::Empty) {
    int cx, cy;
    intersectionXY(cursorR, cursorC, &cx, &cy);
    gomokuart::cursor(renderer, cx, cy, pitch);
  }
}

void GomokuGameActivity::drawStone(int cx, int cy, int radius, bool isBlack) const {
  gomokuart::stone(renderer, cx, cy, radius, isBlack);
}

void GomokuGameActivity::drawWinLine() {
  if (board.winLineStart == GomokuBoard::INVALID_IDX || board.winLineEnd == GomokuBoard::INVALID_IDX) return;
  int x1, y1, x2, y2;
  intersectionXY(board.rowOf(board.winLineStart), board.colOf(board.winLineStart), &x1, &y1);
  intersectionXY(board.rowOf(board.winLineEnd), board.colOf(board.winLineEnd), &x2, &y2);
  // Triple draw for a 2-pixel-wide line that handles both axis-aligned and
  // diagonal directions reasonably (drawLine's lineWidth only thickens along Y).
  renderer.drawLine(x1, y1, x2, y2, true);
  renderer.drawLine(x1 + 1, y1, x2 + 1, y2, true);
  renderer.drawLine(x1, y1 + 1, x2, y2 + 1, true);
}

// ---------- Info panel ----------

void GomokuGameActivity::drawInfoPanel() {
  // One card per side, the side to move inverted (as a selected row is elsewhere in
  // RickyOS); the detail line is the side's stone count, or "thinking" for the AI.
  const int sw = renderer.getScreenWidth();
  const int side = mappedInput.hasTouch() ? int(ds::space::kPageMargin) : int(CONTENT_X);
  const int width = (sw - 2 * side - ds::space::kGutter) / 2;
  const bool live = board.winner == GomokuBoard::Stone::Empty && !board.isFull();
  const GomokuBoard::Stone toMove = live ? board.nextTurn() : GomokuBoard::Stone::Empty;
  static char detail[2][32];
  for (int i = 0; i < 2; ++i) {
    const auto color = i == 0 ? GomokuBoard::Stone::Black : GomokuBoard::Stone::White;
    const uint16_t count = color == GomokuBoard::Stone::Black ? board.blackCount() : board.whiteCount();
    if (aiThinkingArmed && color == toMove)
      snprintf(detail[i], sizeof(detail[i]), "%s", tr(STR_GOMOKU_AI_THINKING));
    else
      snprintf(detail[i], sizeof(detail[i]), "%s %u", tr(STR_GOMOKU_PLAYED), static_cast<unsigned>(count));
    const fui::Rect card{static_cast<int16_t>(side + i * (width + ds::space::kGutter)),
                         static_cast<int16_t>(playersTop()), static_cast<int16_t>(width),
                         static_cast<int16_t>(playersHeight())};
    gomokuart::playerCard(renderer, card, color == GomokuBoard::Stone::Black,
                          color == GomokuBoard::Stone::Black ? tr(STR_GOMOKU_BLACK) : tr(STR_GOMOKU_WHITE), detail[i],
                          color == toMove);
  }
}

// ---------- Mode line ----------

void GomokuGameActivity::drawModeLine() {
  // "AI thinking" now lives on the AI side's card (drawInfoPanel).
}

// ---------- Footer (button hints) ----------

void GomokuGameActivity::drawFooter() {
  const char* backLabel = "";
  const char* confirmLabel = "";
  const char* leftLabel = "";
  const char* rightLabel = "";

  if (state == State::Playing) {
    backLabel = tr(STR_GOMOKU_MENU);
    confirmLabel = tr(STR_GOMOKU_PLACE);
    leftLabel = tr(STR_DIR_LEFT);
    rightLabel = tr(STR_DIR_RIGHT);
  } else if (state == State::GameMenu) {
    backLabel = tr(STR_GAME_RESUME);
    confirmLabel = tr(STR_SELECT);
    leftLabel = tr(STR_DIR_LEFT);
    rightLabel = tr(STR_DIR_RIGHT);
  } else if (state == State::GameOver) {
    backLabel = tr(STR_GOMOKU_MENU);
    confirmLabel = tr(STR_GOMOKU_AGAIN);
  }

  const auto labels = mappedInput.mapLabels(backLabel, confirmLabel, leftLabel, rightLabel);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// ---------- Game Over screen ----------

void GomokuGameActivity::renderGameOver() {
  drawTitleBar();
  const int sw = renderer.getScreenWidth();

  GomokuBoard::Stone winner = resignedFlag ? resignWinner : board.winner;
  const char* titleStr = winner == GomokuBoard::Stone::Black   ? tr(STR_GOMOKU_BLACK_WINS)
                         : winner == GomokuBoard::Stone::White ? tr(STR_GOMOKU_WHITE_WINS)
                                                               : tr(STR_GOMOKU_DRAW);
  char timeBuf[8];
  gameFormatElapsed(elapsedMs, timeBuf, sizeof(timeBuf));
  const char* sizeLabel = (board.boardSize == 9) ? tr(STR_GOMOKU_BOARD_9) : tr(STR_GOMOKU_BOARD_15);
  char sub[96];
  snprintf(sub, sizeof(sub), "%s · %s · %u %s", modeLabel(mode), sizeLabel, static_cast<unsigned>(board.moveCount),
           tr(STR_GOMOKU_MOVES_SUFFIX));

  const uint8_t s = GomokuStore::sizeIndex(board.boardSize);
  const GomokuStats stats = GomokuStore::loadStats();
  char rec[64];
  const bool showRecord = s <= 1;
  if (showRecord) {
    snprintf(rec, sizeof(rec), tr(STR_GOMOKU_RECORD_FMT), static_cast<unsigned>(stats.blackWins[s]),
             static_cast<unsigned>(stats.whiteWins[s]), static_cast<unsigned>(stats.draws[s]));
  }

  const int titleH = renderer.getLineHeight(RickyDesignUi::TITLE);
  const int captionH = renderer.getLineHeight(RickyDesignUi::CAPTION);
  const int valueH = renderer.getLineHeight(RickyDesignUi::HEADLINE);
  const int cardH = 2 * ds::space::k5 + valueH + ds::space::kTextStackGap + captionH;
  const int blockH = titleH + ds::space::kTextStackGap + captionH + ds::space::kSectionGap + cardH +
                     (showRecord ? ds::space::k4 + captionH : 0);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int top = boardAreaTop();
  const int bottom = mappedInput.hasTouch()
                         ? gameTouchActionRect(sw, renderer.getScreenHeight(), metrics.contentSidePadding,
                                               metrics.menuSpacing, metrics.menuRowHeight, 0, 2)
                               .y
                         : renderer.getScreenHeight() - metrics.buttonHintsHeight;
  const int y = gameCenteredBlockY(top, bottom, blockH);
  const int x = ds::space::kPageMargin;
  const int w = sw - 2 * ds::space::kPageMargin;
  using gomokuart::rect;
  RickyDesignUi::text(renderer, rect(x, y, w, titleH), titleStr, RickyDesignUi::TITLE, false, fui::TextAlign::Center);
  RickyDesignUi::secondaryText(renderer, rect(x, y + titleH + ds::space::kTextStackGap, w, captionH), sub,
                               RickyDesignUi::CAPTION, false, fui::TextAlign::Center);

  // Three stat cards: time, best time, games played.
  const int cardsY = y + titleH + ds::space::kTextStackGap + captionH + ds::space::kSectionGap;
  const int cardW = (w - 2 * ds::space::kGutter) / 3;
  char bestBuf[16];
  if (s <= 1 && stats.bestTimeSec[s] > 0) {
    snprintf(bestBuf, sizeof(bestBuf), "%02u:%02u", static_cast<unsigned>(stats.bestTimeSec[s] / 60),
             static_cast<unsigned>(stats.bestTimeSec[s] % 60));
  } else {
    snprintf(bestBuf, sizeof(bestBuf), "--");
  }
  char playedBuf[16];
  const unsigned played = s <= 1 ? stats.blackWins[s] + stats.whiteWins[s] + stats.draws[s] : 0;
  snprintf(playedBuf, sizeof(playedBuf), "%u", played);
  const char* values[3] = {timeBuf, bestBuf, playedBuf};
  const char* names[3] = {tr(STR_GAME_TIME), tr(STR_GAME_BEST_TIME), tr(STR_GOMOKU_PLAYED)};
  for (int i = 0; i < 3; ++i) {
    const int cx = x + i * (cardW + ds::space::kGutter);
    RickyDesignUi::softCard(renderer, rect(cx, cardsY, cardW, cardH), false);
    RickyDesignUi::text(renderer, rect(cx, cardsY + ds::space::k5, cardW, valueH), values[i], RickyDesignUi::HEADLINE,
                        false, fui::TextAlign::Center);
    RickyDesignUi::secondaryText(renderer,
                                 rect(cx, cardsY + ds::space::k5 + valueH + ds::space::kTextStackGap, cardW, captionH),
                                 names[i], RickyDesignUi::CAPTION, false, fui::TextAlign::Center);
  }
  if (showRecord) {
    RickyDesignUi::secondaryText(renderer, rect(x, cardsY + cardH + ds::space::k4, w, captionH), rec,
                                 RickyDesignUi::CAPTION, false, fui::TextAlign::Center);
  }

  if (mappedInput.hasTouch()) {
    GUI.drawActionButton(
        renderer,
        gameTouchActionRect(renderer.getScreenWidth(), renderer.getScreenHeight(), metrics.contentSidePadding,
                            metrics.menuSpacing, metrics.menuRowHeight, 0, 2),
        tr(STR_GOMOKU_AGAIN), true);
    GUI.drawActionButton(
        renderer,
        gameTouchActionRect(renderer.getScreenWidth(), renderer.getScreenHeight(), metrics.contentSidePadding,
                            metrics.menuSpacing, metrics.menuRowHeight, 1, 2),
        tr(STR_GAME_HOME));
  }

  drawFooter();
}
