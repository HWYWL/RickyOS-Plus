#include "LibraryFoldersActivity.h"

#include <I18n.h>

#include <algorithm>
#include <string>

namespace fui = freeink::ui;

namespace {
// True when `path` is `folder` itself or lies under it ("" is above everything).
bool within(const std::string& path, const std::string& folder) {
  if (folder.empty()) return true;
  return path.size() >= folder.size() && path.compare(0, folder.size(), folder) == 0 &&
         (path.size() == folder.size() || path[folder.size()] == '/');
}

// The name of the child of `folder` that `path` lies in, or "" when `path` is `folder`.
std::string childName(const std::string& path, const std::string& folder) {
  size_t start = folder.size();
  if (start < path.size() && path[start] == '/') ++start;
  if (start >= path.size()) return {};
  const size_t end = path.find('/', start);
  return path.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string join(const std::string& folder, const std::string& name) {
  return folder.empty() && !name.empty() && name[0] != '/' ? "/" + name : folder + "/" + name;
}
}  // namespace

const char* LibraryFoldersActivity::headerTitle() const { return title.c_str(); }

void LibraryFoldersActivity::onEnter() {
  RenderLock lock(*this);
  UiListActivity::onEnter();
  waitForButtons = mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                   mappedInput.isPressed(MappedInputManager::Button::Back);
  // Skip levels that hold no books and a single subfolder ("/" -> "/books"), so the
  // first screen is the one with a choice on it.
  root.clear();
  for (;;) {
    std::string only;
    bool books = false, several = false;
    for (const auto& folder : folders) {
      if (!within(folder.path, root)) continue;
      const std::string name = childName(folder.path, root);
      if (name.empty()) {
        books = true;
        break;
      }
      if (only.empty()) only = name;
      else if (only != name) several = true;
    }
    if (books || several || only.empty()) break;
    root = join(root, only);
  }
  // Open where the shelf's folder lives, so changing it is one step.
  std::string start = root;
  if (!selected.empty() && within(selected, root)) {
    const size_t slash = selected.rfind('/');
    start = slash != std::string::npos && slash > 0 ? selected.substr(0, slash) : root;
    if (!within(start, root)) start = root;
  }
  show(start);
  requestUpdate();
}

void LibraryFoldersActivity::show(const std::string& path) {
  level = path;
  rows.clear();
  uint32_t total = 0;
  for (const auto& folder : folders)
    if (within(folder.path, level)) total += folder.count;
  const bool top = level == root;
  const size_t slash = level.rfind('/');
  title = top ? std::string(tr(STR_LIBRARY_FOLDERS))
              : (slash == std::string::npos ? level : level.substr(slash + 1));
  rows.push_back({top ? tr(STR_LIBRARY_ALL_FOLDERS) : tr(STR_LIBRARY_THIS_FOLDER), top ? std::string() : level,
                  std::to_string(total), total, false});
  // Children of this level, with their whole subtree's books.
  std::vector<Row> children;
  for (const auto& folder : folders) {
    if (!within(folder.path, level)) continue;
    const std::string name = childName(folder.path, level);
    if (name.empty()) continue;
    const std::string path = join(level, name);
    auto it = std::find_if(children.begin(), children.end(), [&](const Row& row) { return row.path == path; });
    if (it == children.end()) {
      children.push_back({name, path, std::string(), 0, false});
      it = children.end() - 1;
    }
    it->count += folder.count;
    if (folder.path != path) it->opens = true;
  }
  std::sort(children.begin(), children.end(), [](const Row& a, const Row& b) { return a.label < b.label; });
  for (auto& child : children) {
    child.value = std::to_string(child.count) + (child.opens ? "  ›" : "");
    rows.push_back(std::move(child));
  }
  int row = 0;
  for (size_t i = 0; i < rows.size(); ++i)
    if (!selected.empty() && rows[i].path == selected) row = static_cast<int>(i);
  nav.reset(row);
}

void LibraryFoldersActivity::provideRow(void* ctx, uint16_t index, fui::ListItem& item) {
  auto& self = *static_cast<LibraryFoldersActivity*>(ctx);
  item.actionValue = index;
  if (index >= self.rows.size()) return;
  const Row& row = self.rows[index];
  item.label = row.label.c_str();
  item.value = (index == 0 ? row.path == self.selected : (!row.opens && row.path == self.selected))
                   ? tr(STR_SELECTED)
                   : row.value.c_str();
}

void LibraryFoldersActivity::buildScreen(UiScreen& screen) {
  const auto bounds = pageContentRect();
  screen.setContentMarginFromScreen(
      fui::Insets{static_cast<int16_t>(bounds.y), 0,
                  static_cast<int16_t>(renderer.getScreenHeight() - bounds.y - bounds.height), 0});
  fui::ListProps props;
  props.rowProvider = &provideRow;
  props.rowProviderCtx = this;
  props.count = listCount();
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  syncListViewport(screen, props);
  screen.list(props);
}

void LibraryFoldersActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  const Row& row = rows[index];
  if (row.opens) {
    RenderLock lock(*this);
    show(row.path);
    app.clearTapFlash();
    requestUpdate();
    return;
  }
  setResult(ActivityResult{FilePathResult{row.path}});
  app.clearTapFlash();
  finish();
}

void LibraryFoldersActivity::onBackButton() {
  if (level != root) {
    RenderLock lock(*this);
    const size_t slash = level.rfind('/');
    std::string up = slash != std::string::npos && slash > 0 ? level.substr(0, slash) : root;
    show(within(up, root) ? up : root);
    app.clearTapFlash();
    requestUpdate();
    return;
  }
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  app.clearTapFlash();
  finish();
}

bool LibraryFoldersActivity::handleCustomInput() {
  if (!waitForButtons) return false;
  waitForButtons = mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                   mappedInput.isPressed(MappedInputManager::Button::Back);
  return true;
}
