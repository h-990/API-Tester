#include <raylib.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "audit_engine.h"
#include "report_writer.h"

namespace {

struct KeyField {
  std::string id;
  std::string label;
  std::string value;
};

struct AppPaths {
  std::filesystem::path root_dir;
  std::filesystem::path config_file;
  std::filesystem::path reports_dir;
  std::filesystem::path logs_dir;
};

struct SharedState {
  std::mutex mutex;
  llaudit::AuditReport last_report;
  bool has_report = false;
  std::vector<std::string> logs;
  std::string summary_text;
  std::string status_text;
  std::string last_json_path;
  std::string last_txt_path;
  std::string last_log_path;
};

std::string Trim(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

std::filesystem::path WorkspaceHintPath() {
  const char* home = std::getenv("HOME");
  if (home && *home) {
    return std::filesystem::path(home) / ".api_tester" / "last_workspace.txt";
  }
  return std::filesystem::path(".api_tester") / "last_workspace.txt";
}

bool SaveWorkspaceHint(const std::string& workspace, std::string& error) {
  try {
    const auto hint = WorkspaceHintPath();
    std::filesystem::create_directories(hint.parent_path());
    std::ofstream ofs(hint, std::ios::out | std::ios::trunc);
    if (!ofs) {
      error = "Failed to open workspace hint file for writing.";
      return false;
    }
    ofs << workspace;
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  }
}

bool LoadWorkspaceHint(std::string& workspace, std::string& error) {
  try {
    const auto hint = WorkspaceHintPath();
    if (!std::filesystem::exists(hint)) return true;
    std::ifstream ifs(hint);
    if (!ifs) {
      error = "Failed to open workspace hint file for reading.";
      return false;
    }
    std::string content;
    std::getline(ifs, content);
    workspace = Trim(content);
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  }
}

AppPaths BuildPaths(const std::string& workspace_input) {
  AppPaths out;
  out.root_dir = std::filesystem::absolute(std::filesystem::path(workspace_input));
  out.config_file = out.root_dir / "config" / "api_keys.json";
  out.reports_dir = out.root_dir / "reports";
  out.logs_dir = out.root_dir / "logs";
  return out;
}

bool EnsureWorkspace(const AppPaths& paths, std::string& error) {
  try {
    std::filesystem::create_directories(paths.root_dir);
    std::filesystem::create_directories(paths.config_file.parent_path());
    std::filesystem::create_directories(paths.reports_dir);
    std::filesystem::create_directories(paths.logs_dir);
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  }
}

std::string MaskKey(const std::string& k) {
  if (k.size() <= 10) return k;
  return k.substr(0, 6) + "..." + k.substr(k.size() - 4);
}

float ClampF(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream iss(text);
  std::string line;
  while (std::getline(iss, line)) lines.push_back(line);
  if (lines.empty()) lines.push_back({});
  return lines;
}

void DrawCard(const Rectangle& rect, Color fill, Color border) {
  DrawRectangleRounded(rect, 0.06f, 10, fill);
  DrawRectangleRoundedLinesEx(rect, 0.06f, 10, 1.0f, border);
}

void DrawPanelText(const Rectangle& panel, const std::vector<std::string>& lines, float& scroll,
                   int font_size = 18, Color color = {220, 220, 220, 255}) {
  const int line_height = font_size + 4;
  const float content_height = static_cast<float>(lines.size() * line_height + 16);
  const float max_scroll = std::max(0.0f, content_height - panel.height);

  if (CheckCollisionPointRec(GetMousePosition(), panel)) {
    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f) {
      scroll = ClampF(scroll - wheel * 28.0f, 0.0f, max_scroll);
    }
  }

  BeginScissorMode(static_cast<int>(panel.x), static_cast<int>(panel.y), static_cast<int>(panel.width),
                   static_cast<int>(panel.height));
  float y = panel.y + 8.0f - scroll;
  for (const auto& line : lines) {
    if (y + line_height > panel.y && y < panel.y + panel.height) {
      DrawText(line.c_str(), static_cast<int>(panel.x + 8), static_cast<int>(y), font_size, color);
    }
    y += static_cast<float>(line_height);
  }
  EndScissorMode();
}

bool DrawButton(const Rectangle& rect, const std::string& label, bool enabled = true) {
  const Vector2 mouse = GetMousePosition();
  const bool hover = enabled && CheckCollisionPointRec(mouse, rect);
  const bool click = hover && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);

  Color fill = {44, 90, 101, 255};
  if (!enabled) {
    fill = {54, 57, 64, 255};
  } else if (hover) {
    fill = {57, 120, 132, 255};
  }

  DrawRectangleRounded(rect, 0.15f, 8, fill);
  DrawRectangleRoundedLinesEx(rect, 0.15f, 8, 1.0f, {112, 182, 196, 255});

  const int fs = 17;
  const int tw = MeasureText(label.c_str(), fs);
  DrawText(label.c_str(), static_cast<int>(rect.x + (rect.width - tw) * 0.5f),
           static_cast<int>(rect.y + (rect.height - fs) * 0.5f), fs,
           enabled ? Color{238, 244, 248, 255} : Color{149, 155, 165, 255});

  return click;
}

bool SaveConfig(const std::vector<KeyField>& fields, const std::filesystem::path& config_file,
                std::string& error) {
  try {
    std::filesystem::create_directories(config_file.parent_path());
    nlohmann::json j;
    for (const auto& field : fields) j[field.id] = field.value;
    std::ofstream ofs(config_file, std::ios::out | std::ios::trunc);
    if (!ofs) {
      error = "Failed to open config file for writing.";
      return false;
    }
    ofs << j.dump(2);
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  }
}

bool LoadConfig(std::vector<KeyField>& fields, const std::filesystem::path& config_file,
                std::string& error) {
  try {
    if (!std::filesystem::exists(config_file)) return true;
    std::ifstream ifs(config_file);
    if (!ifs) {
      error = "Failed to open config file for reading.";
      return false;
    }
    nlohmann::json j = nlohmann::json::parse(ifs, nullptr, false);
    if (!j.is_object()) {
      error = "Config file JSON is invalid.";
      return false;
    }
    for (auto& field : fields) {
      if (j.contains(field.id) && j[field.id].is_string()) field.value = j[field.id].get<std::string>();
    }
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    return false;
  }
}

std::map<std::string, std::string> KeysToMap(const std::vector<KeyField>& fields) {
  std::map<std::string, std::string> out;
  for (const auto& field : fields) out[field.id] = field.value;
  return out;
}

std::string BuildFieldText(const std::string& value, bool show_keys) {
  return show_keys ? value : MaskKey(value);
}

void PasteIntoField(std::string& target, std::size_t max_len = 1024) {
  const char* clip = GetClipboardText();
  if (!clip) return;

  for (const unsigned char ch : std::string(clip)) {
    if (target.size() >= max_len) break;
    if (ch == '\n' || ch == '\r' || ch == '\t') continue;
    if (ch >= 32 && ch != 127) {
      target.push_back(static_cast<char>(ch));
    }
  }
}

void HandleTextInput(std::string& target, std::size_t max_len = 1024) {
  bool paste_shortcut = false;
#if defined(__APPLE__)
  const bool mod_down = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER);
#else
  const bool mod_down = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
#endif
  if (mod_down && IsKeyPressed(KEY_V)) paste_shortcut = true;
  if ((IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) && IsKeyPressed(KEY_INSERT)) {
    paste_shortcut = true;
  }
  if (paste_shortcut) {
    PasteIntoField(target, max_len);
  }

  int key = GetCharPressed();
  while (key > 0) {
    if (key >= 32 && key <= 126 && target.size() < max_len) {
      target.push_back(static_cast<char>(key));
    }
    key = GetCharPressed();
  }
  if (IsKeyPressed(KEY_BACKSPACE) && !target.empty()) {
    target.pop_back();
  }
}

}  // namespace

int main() {
  SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
  InitWindow(1580, 980, "API-Tester - Cross Platform GUI");
  SetTargetFPS(60);

  std::vector<KeyField> fields = {
      {"openrouter", "OpenRouter API Key", ""},
      {"google_ai_studio", "Google AI Studio API Key", ""},
      {"mistral", "Mistral API Key", ""},
      {"vercel", "Vercel API Key", ""},
      {"groq", "Groq API Key", ""},
      {"cohere", "Cohere API Key", ""},
      {"ai21", "AI21 API Key", ""},
      {"github_chatgpt", "GitHub PAT (chatgpt)", ""},
      {"github_chatgpt5", "GitHub PAT (chatgpt5)", ""},
      {"github_deepseek", "GitHub PAT (deepseek)", ""},
      {"github_jamba", "GitHub PAT (jamba)", ""},
  };

  SharedState shared;
  std::atomic<bool> audit_running{false};
  std::atomic<bool> cancel_requested{false};
  std::thread worker;

  float keys_scroll = 0.0f;
  float summary_scroll = 0.0f;
  float logs_scroll = 0.0f;
  int active_field = -1;
  bool workspace_input_active = false;
  bool show_keys = false;

  std::string workspace_input;
  AppPaths workspace_paths;
  bool workspace_ready = false;

  std::string workspace_hint_error;
  if (!LoadWorkspaceHint(workspace_input, workspace_hint_error)) {
    std::scoped_lock lock(shared.mutex);
    shared.status_text = "Workspace hint warning: " + workspace_hint_error;
  }
  if (workspace_input.empty()) {
    workspace_input = std::filesystem::current_path().string();
  }

  {
    std::scoped_lock lock(shared.mutex);
    if (shared.status_text.empty()) {
      shared.status_text = "Set a working directory, then click Apply Workspace.";
    }
  }

  auto apply_workspace = [&](bool load_keys) {
    const std::string cleaned = Trim(workspace_input);
    if (cleaned.empty()) {
      std::scoped_lock lock(shared.mutex);
      shared.status_text = "Workspace path is empty.";
      workspace_ready = false;
      return;
    }

    auto candidate = BuildPaths(cleaned);
    std::string error;
    if (!EnsureWorkspace(candidate, error)) {
      std::scoped_lock lock(shared.mutex);
      shared.status_text = "Workspace setup failed: " + error;
      workspace_ready = false;
      return;
    }

    workspace_paths = candidate;
    workspace_ready = true;

    std::string hint_error;
    SaveWorkspaceHint(workspace_paths.root_dir.string(), hint_error);

    if (load_keys) {
      std::string load_error;
      if (!LoadConfig(fields, workspace_paths.config_file, load_error) && !load_error.empty()) {
        std::scoped_lock lock(shared.mutex);
        shared.status_text = "Workspace applied, key-load warning: " + load_error;
        return;
      }
    }

    std::scoped_lock lock(shared.mutex);
    shared.status_text = "Workspace applied: " + workspace_paths.root_dir.string();
  };

  while (!WindowShouldClose()) {
    if (!audit_running.load() && worker.joinable()) {
      worker.join();
    }

    if (workspace_input_active) {
      HandleTextInput(workspace_input, 2048);
    } else if (active_field >= 0 && active_field < static_cast<int>(fields.size())) {
      HandleTextInput(fields[active_field].value, 1024);
    }

    const int sw = GetScreenWidth();
    const int sh = GetScreenHeight();

    const float margin = 14.0f;
    const float header_h = 86.0f;

    const Rectangle header = {margin, margin, static_cast<float>(sw) - margin * 2.0f, header_h};

    const float content_top = header.y + header.height + 12.0f;
    const float content_h = static_cast<float>(sh) - content_top - margin;
    const float left_w = std::max(470.0f, std::min(560.0f, static_cast<float>(sw) * 0.36f));

    const Rectangle left_panel = {margin, content_top, left_w, content_h};
    const Rectangle right_panel = {left_panel.x + left_panel.width + 12.0f, content_top,
                                   static_cast<float>(sw) - (left_panel.x + left_panel.width + margin + 12.0f),
                                   content_h};

    const Rectangle workspace_card = {left_panel.x + 12.0f, left_panel.y + 12.0f, left_panel.width - 24.0f, 142.0f};
    const Rectangle controls_card = {left_panel.x + 12.0f, workspace_card.y + workspace_card.height + 12.0f,
                                     left_panel.width - 24.0f, 186.0f};
    const Rectangle fields_area = {left_panel.x + 12.0f, controls_card.y + controls_card.height + 42.0f,
                                   left_panel.width - 24.0f,
                                   left_panel.y + left_panel.height - (controls_card.y + controls_card.height + 54.0f)};

    const Rectangle summary_panel = {right_panel.x + 12.0f, right_panel.y + 54.0f, right_panel.width - 24.0f,
                                     right_panel.height * 0.56f};
    const Rectangle logs_panel = {right_panel.x + 12.0f, summary_panel.y + summary_panel.height + 42.0f,
                                  right_panel.width - 24.0f,
                                  right_panel.y + right_panel.height - (summary_panel.y + summary_panel.height + 54.0f)};

    BeginDrawing();

    DrawRectangleGradientEx({0, 0, static_cast<float>(sw), static_cast<float>(sh)},
                            Color{14, 20, 28, 255}, Color{16, 32, 40, 255}, Color{11, 21, 29, 255},
                            Color{9, 14, 20, 255});
    DrawCircleV({sw * 0.83f, 72.0f}, 220.0f, {55, 168, 188, 28});
    DrawCircleV({sw * 0.11f, static_cast<float>(sh) * 0.82f}, 260.0f, {72, 137, 208, 20});

    DrawCard(header, {27, 41, 54, 230}, {82, 139, 168, 255});
    DrawText("API-Tester", static_cast<int>(header.x + 18), static_cast<int>(header.y + 16), 34,
             {239, 245, 250, 255});
    DrawText("Modern cross-platform auditor for API limits, models, working checks and quality tests",
             static_cast<int>(header.x + 20), static_cast<int>(header.y + 54), 18, {169, 203, 220, 255});

    const std::string workspace_header = "Workspace: " + (workspace_ready ? workspace_paths.root_dir.string() : "Not applied yet");
    DrawText(workspace_header.c_str(), static_cast<int>(header.x + header.width - 760),
             static_cast<int>(header.y + 20), 16, workspace_ready ? Color{164, 239, 191, 255} : Color{255, 205, 132, 255});

    DrawCard(left_panel, {24, 30, 41, 235}, {66, 88, 114, 255});
    DrawCard(right_panel, {24, 30, 41, 235}, {66, 88, 114, 255});

    DrawCard(workspace_card, {24, 42, 52, 240}, {78, 148, 172, 255});
    DrawText("Working Directory", static_cast<int>(workspace_card.x + 14), static_cast<int>(workspace_card.y + 12),
             22, {225, 241, 247, 255});

    const Rectangle workspace_input_rect = {workspace_card.x + 12, workspace_card.y + 48,
                                            workspace_card.width - 24, 36};
    DrawRectangleRounded(workspace_input_rect, 0.12f, 8,
                         workspace_input_active ? Color{54, 77, 91, 255} : Color{38, 55, 68, 255});
    DrawRectangleRoundedLinesEx(workspace_input_rect, 0.12f, 8, 1.0f,
                                workspace_input_active ? Color{123, 204, 228, 255} : Color{95, 143, 165, 255});

    std::string shown_workspace = workspace_input;
    if (shown_workspace.size() > 110) {
      shown_workspace = shown_workspace.substr(0, 107) + "...";
    }
    DrawText(shown_workspace.c_str(), static_cast<int>(workspace_input_rect.x + 8),
             static_cast<int>(workspace_input_rect.y + 9), 17, {227, 236, 242, 255});

    const float ws_btn_gap = 10.0f;
    const float ws_btn_w = (workspace_card.width - 12 * 2 - ws_btn_gap) / 2.0f;
    const Rectangle ws_apply = {workspace_card.x + 12, workspace_card.y + 94, ws_btn_w, 34};
    const Rectangle ws_use_cwd = {ws_apply.x + ws_btn_w + ws_btn_gap, workspace_card.y + 94, ws_btn_w, 34};

    if (DrawButton(ws_apply, "Apply Workspace", !audit_running.load())) {
      workspace_input_active = false;
      active_field = -1;
      apply_workspace(true);
    }

    if (DrawButton(ws_use_cwd, "Use Current Dir", !audit_running.load())) {
      workspace_input = std::filesystem::current_path().string();
      workspace_input_active = true;
      active_field = -1;
    }

    DrawCard(controls_card, {22, 35, 44, 240}, {76, 121, 141, 255});
    DrawText("Controls", static_cast<int>(controls_card.x + 14), static_cast<int>(controls_card.y + 10), 22,
             {221, 236, 245, 255});

    const float c_gap = 10.0f;
    const float c_btn_w = (controls_card.width - 14 * 2 - c_gap) / 2.0f;
    const float c_btn_h = 32.0f;
    const float c_x1 = controls_card.x + 14;
    const float c_x2 = c_x1 + c_btn_w + c_gap;
    const float c_y0 = controls_card.y + 44;

    if (DrawButton({c_x1, c_y0, c_btn_w, c_btn_h}, "Load Keys", workspace_ready && !audit_running.load())) {
      std::string error;
      if (LoadConfig(fields, workspace_paths.config_file, error)) {
        std::scoped_lock lock(shared.mutex);
        shared.status_text = "Loaded API keys from " + workspace_paths.config_file.string();
      } else {
        std::scoped_lock lock(shared.mutex);
        shared.status_text = "Load failed: " + error;
      }
    }

    if (DrawButton({c_x2, c_y0, c_btn_w, c_btn_h}, "Save Keys", workspace_ready && !audit_running.load())) {
      std::string error;
      if (SaveConfig(fields, workspace_paths.config_file, error)) {
        std::scoped_lock lock(shared.mutex);
        shared.status_text = "Saved API keys to " + workspace_paths.config_file.string();
      } else {
        std::scoped_lock lock(shared.mutex);
        shared.status_text = "Save failed: " + error;
      }
    }

    if (DrawButton({c_x1, c_y0 + 40, c_btn_w, c_btn_h}, "Run Full Audit",
                   workspace_ready && !audit_running.load())) {
      cancel_requested.store(false);
      audit_running.store(true);
      active_field = -1;
      workspace_input_active = false;

      {
        std::scoped_lock lock(shared.mutex);
        shared.logs.clear();
        shared.summary_text.clear();
        shared.status_text = "Audit started...";
      }

      const auto keys_map = KeysToMap(fields);
      const AppPaths paths_copy = workspace_paths;
      worker = std::thread([&shared, &audit_running, &cancel_requested, keys_map, paths_copy]() {
        try {
          llaudit::AuditEngine engine;
          auto report = engine.Run(
              keys_map,
              [&shared](const std::string& line) {
                std::scoped_lock lock(shared.mutex);
                shared.logs.push_back(line);
              },
              cancel_requested);

          const auto run_log_path = llaudit::WriteRunLog(report, paths_copy.logs_dir);

          {
            std::scoped_lock lock(shared.mutex);
            shared.last_report = report;
            shared.has_report = true;
            shared.summary_text = llaudit::BuildSummaryText(report);
            shared.last_log_path = run_log_path;
            shared.status_text = cancel_requested.load() ? "Audit canceled." : "Audit completed.";
            if (!run_log_path.empty()) {
              shared.status_text += " Run log: " + run_log_path;
            }
          }
        } catch (const std::exception& ex) {
          std::scoped_lock lock(shared.mutex);
          shared.status_text = std::string("Audit failed: ") + ex.what();
        } catch (...) {
          std::scoped_lock lock(shared.mutex);
          shared.status_text = "Audit failed: unknown error.";
        }
        audit_running.store(false);
      });
    }

    if (DrawButton({c_x2, c_y0 + 40, c_btn_w, c_btn_h}, "Stop", audit_running.load())) {
      cancel_requested.store(true);
      std::scoped_lock lock(shared.mutex);
      shared.status_text = "Cancellation requested...";
    }

    bool has_report = false;
    llaudit::AuditReport report_copy;
    {
      std::scoped_lock lock(shared.mutex);
      has_report = shared.has_report;
      if (has_report) report_copy = shared.last_report;
    }

    if (DrawButton({c_x1, c_y0 + 80, c_btn_w, c_btn_h}, "Export JSON", workspace_ready && has_report)) {
      const auto path = llaudit::WriteJsonReport(report_copy, workspace_paths.reports_dir);
      std::scoped_lock lock(shared.mutex);
      shared.last_json_path = path;
      if (!path.empty()) {
        shared.status_text = "JSON report exported: " + path;
      } else {
        shared.status_text = "Failed to export JSON report.";
      }
    }

    if (DrawButton({c_x2, c_y0 + 80, c_btn_w, c_btn_h}, "Export TXT", workspace_ready && has_report)) {
      const auto path = llaudit::WriteTextReport(report_copy, workspace_paths.reports_dir);
      std::scoped_lock lock(shared.mutex);
      shared.last_txt_path = path;
      if (!path.empty()) {
        shared.status_text = "TXT report exported: " + path;
      } else {
        shared.status_text = "Failed to export TXT report.";
      }
    }

    if (DrawButton({c_x1, c_y0 + 120, c_btn_w, c_btn_h}, show_keys ? "Hide Keys" : "Show Keys")) {
      show_keys = !show_keys;
    }

    if (DrawButton({c_x2, c_y0 + 120, c_btn_w, c_btn_h}, "Clear Logs")) {
      std::scoped_lock lock(shared.mutex);
      shared.logs.clear();
      shared.status_text = "Logs cleared.";
    }

    DrawText("API Key Inputs", static_cast<int>(left_panel.x + 16), static_cast<int>(fields_area.y - 28), 21,
             {220, 238, 247, 255});
    DrawCard(fields_area, {20, 26, 36, 245}, {67, 91, 118, 255});

    const float row_h = 66.0f;
    const float keys_content_h = static_cast<float>(fields.size()) * row_h + 10.0f;
    const float max_keys_scroll = std::max(0.0f, keys_content_h - fields_area.height);
    if (CheckCollisionPointRec(GetMousePosition(), fields_area)) {
      keys_scroll = ClampF(keys_scroll - GetMouseWheelMove() * 28.0f, 0.0f, max_keys_scroll);
    }

    bool clicked_input = false;
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(GetMousePosition(), workspace_input_rect)) {
      workspace_input_active = true;
      active_field = -1;
      clicked_input = true;
    }

    BeginScissorMode(static_cast<int>(fields_area.x), static_cast<int>(fields_area.y),
                     static_cast<int>(fields_area.width), static_cast<int>(fields_area.height));

    for (int i = 0; i < static_cast<int>(fields.size()); ++i) {
      const float y = fields_area.y + 8.0f + i * row_h - keys_scroll;
      Rectangle input = {fields_area.x + 10, y + 22, fields_area.width - 20, 34};

      if (y + row_h < fields_area.y || y > fields_area.y + fields_area.height) continue;

      DrawText(fields[i].label.c_str(), static_cast<int>(input.x), static_cast<int>(y), 16,
               {165, 192, 220, 255});

      const bool active = (i == active_field && !workspace_input_active);
      DrawRectangleRounded(input, 0.12f, 8, active ? Color{52, 75, 94, 255} : Color{35, 46, 58, 255});
      DrawRectangleRoundedLinesEx(input, 0.12f, 8, 1.0f,
                                  active ? Color{117, 198, 223, 255} : Color{83, 118, 148, 255});

      std::string shown = BuildFieldText(fields[i].value, show_keys);
      if (shown.size() > 100) {
        shown = shown.substr(0, 97) + "...";
      }

      DrawText(shown.c_str(), static_cast<int>(input.x + 8), static_cast<int>(input.y + 8), 17,
               {227, 235, 242, 255});

      if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(GetMousePosition(), input)) {
        active_field = i;
        workspace_input_active = false;
        clicked_input = true;
      }
    }

    EndScissorMode();

    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && !clicked_input &&
        !CheckCollisionPointRec(GetMousePosition(), fields_area) &&
        !CheckCollisionPointRec(GetMousePosition(), workspace_input_rect)) {
      active_field = -1;
      workspace_input_active = false;
    }

    DrawText("Audit Summary", static_cast<int>(summary_panel.x), static_cast<int>(right_panel.y + 16), 24,
             {231, 244, 251, 255});
    DrawCard(summary_panel, {20, 27, 37, 245}, {68, 100, 133, 255});

    DrawText("Live Logs", static_cast<int>(logs_panel.x), static_cast<int>(summary_panel.y + summary_panel.height + 8),
             24, {231, 244, 251, 255});
    DrawCard(logs_panel, {20, 27, 37, 245}, {68, 100, 133, 255});

    std::vector<std::string> summary_lines;
    std::vector<std::string> log_lines;
    std::string status_line;
    std::string last_json;
    std::string last_txt;
    std::string last_log;

    {
      std::scoped_lock lock(shared.mutex);
      summary_lines = SplitLines(shared.summary_text.empty() ? "No audit run yet." : shared.summary_text);
      log_lines = shared.logs;
      if (log_lines.empty()) log_lines.push_back("No logs yet.");
      status_line = shared.status_text;
      last_json = shared.last_json_path;
      last_txt = shared.last_txt_path;
      last_log = shared.last_log_path;
    }

    DrawPanelText(summary_panel, summary_lines, summary_scroll, 18, {219, 229, 237, 255});
    DrawPanelText(logs_panel, log_lines, logs_scroll, 17, {212, 224, 235, 255});

    const std::string run_state = audit_running.load() ? "RUNNING" : "IDLE";
    DrawText(("State: " + run_state).c_str(), static_cast<int>(left_panel.x + 16),
             static_cast<int>(left_panel.y + left_panel.height - 34), 18,
             audit_running.load() ? Color{255, 205, 125, 255} : Color{166, 239, 193, 255});

    DrawText(status_line.c_str(), static_cast<int>(right_panel.x + 12),
             static_cast<int>(right_panel.y + right_panel.height - 30), 16,
             {186, 210, 229, 255});

    if (workspace_ready) {
      DrawText(("Config: " + workspace_paths.config_file.string()).c_str(),
               static_cast<int>(left_panel.x + 16), static_cast<int>(left_panel.y + left_panel.height - 56), 14,
               {130, 170, 197, 255});
    }

    if (!last_txt.empty()) {
      DrawText(("Last TXT: " + last_txt).c_str(), static_cast<int>(right_panel.x + 12),
               static_cast<int>(right_panel.y + right_panel.height - 50), 14, {140, 177, 209, 255});
    }
    if (!last_json.empty()) {
      DrawText(("Last JSON: " + last_json).c_str(), static_cast<int>(right_panel.x + 12),
               static_cast<int>(right_panel.y + right_panel.height - 68), 14, {140, 177, 209, 255});
    }
    if (!last_log.empty()) {
      DrawText(("Last LOG: " + last_log).c_str(), static_cast<int>(right_panel.x + 12),
               static_cast<int>(right_panel.y + right_panel.height - 86), 14, {140, 177, 209, 255});
    }

    EndDrawing();
  }

  cancel_requested.store(true);
  if (worker.joinable()) worker.join();
  CloseWindow();
  return 0;
}
