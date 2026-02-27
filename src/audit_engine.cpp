#include "audit_engine.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <ctime>
#include <functional>
#include <iomanip>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>

namespace llaudit {
namespace {

struct HttpResponse {
  long status = -1;
  long latency_ms = -1;
  std::string body;
  std::map<std::string, std::string> headers;
  std::string error;
};

constexpr int kSnippetLen = 500;

const std::array<std::pair<std::string, std::string>, 3> kPromptSuite = {
    std::pair{"reasoning",
              "Answer in one sentence: If 5 machines make 5 widgets in 5 "
              "minutes, how long would 100 machines take to make 100 widgets?"},
    std::pair{"coding",
              "Fix this Python bug and return only corrected function "
              "code:\n\ndef is_palindrome(s):\n    s = s.lower().replace(' ', "
              "'')\n    return s == s.reverse()\n"},
    std::pair{"axui",
              "You are given an AX tree:\nAXWindow\n  AXGroup 'Checkout "
              "Form'\n    AXTextField id=email label='Email'\n    AXButton "
              "id=btn_continue name='Continue' enabled=true\n  AXSheet "
              "'Newsletter' modal=true\n    AXButton id=close_popup name='No "
              "thanks' enabled=true\nTask: return JSON with first_action and "
              "target_id that actually completes checkout flow."},
};

std::string NowUtc() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto t = system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

std::string ToLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

std::string Trim(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

std::string Snippet(const std::string &s, std::size_t limit = kSnippetLen) {
  if (s.size() <= limit)
    return s;
  return s.substr(0, limit);
}

#if !defined(_WIN32)
size_t WriteBodyCallback(void *contents, size_t size, size_t nmemb,
                         void *userp) {
  const size_t total_size = size * nmemb;
  auto *body = static_cast<std::string *>(userp);
  body->append(static_cast<char *>(contents), total_size);
  return total_size;
}

size_t HeaderCallback(char *buffer, size_t size, size_t nitems,
                      void *userdata) {
  const size_t total = size * nitems;
  std::string line(buffer, total);
  auto *out = static_cast<std::map<std::string, std::string> *>(userdata);
  const auto pos = line.find(':');
  if (pos != std::string::npos) {
    const std::string key = ToLower(Trim(line.substr(0, pos)));
    const std::string value = Trim(line.substr(pos + 1));
    if (!key.empty()) {
      (*out)[key] = value;
    }
  }
  return total;
}

HttpResponse Request(const std::string &method, const std::string &url,
                     const std::vector<std::string> &headers,
                     const std::optional<std::string> &body,
                     long timeout_seconds = 60) {
  HttpResponse result;
  CURL *curl = curl_easy_init();
  if (!curl) {
    result.error = "curl_easy_init failed";
    return result;
  }

  struct curl_slist *header_list = nullptr;
  header_list = curl_slist_append(header_list, "User-Agent: llm-audit-gui/1.0");
  for (const auto &h : headers) {
    header_list = curl_slist_append(header_list, h.c_str());
  }

  std::string response_body;
  std::map<std::string, std::string> response_headers;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteBodyCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);

  if (method == "POST") {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
  } else if (method != "GET") {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  }

  if (body.has_value()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(body->size()));
  }

  const auto start = std::chrono::steady_clock::now();
  const CURLcode code = curl_easy_perform(curl);
  const auto end = std::chrono::steady_clock::now();
  result.latency_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
          .count();

  if (code != CURLE_OK) {
    result.error = curl_easy_strerror(code);
  }

  long http_status = -1;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  result.status = http_status;
  result.body = std::move(response_body);
  result.headers = std::move(response_headers);

  curl_slist_free_all(header_list);
  curl_easy_cleanup(curl);
  return result;
}
#else
std::wstring Utf8ToWide(const std::string &s) {
  if (s.empty())
    return L"";
  const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
  if (len <= 0)
    return L"";
  std::wstring out(static_cast<std::size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      out.data(), len);
  return out;
}

std::string WideToUtf8(const std::wstring &s) {
  if (s.empty())
    return {};
  const int len =
      WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                          nullptr, 0, nullptr, nullptr);
  if (len <= 0)
    return {};
  std::string out(static_cast<std::size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      out.data(), len, nullptr, nullptr);
  return out;
}

std::string WinErrText(DWORD code) {
  LPWSTR wbuf = nullptr;
  const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                      FORMAT_MESSAGE_FROM_SYSTEM |
                      FORMAT_MESSAGE_IGNORE_INSERTS;
  const DWORD got = FormatMessageW(flags, nullptr, code, 0,
                                   reinterpret_cast<LPWSTR>(&wbuf), 0, nullptr);
  std::string out = "winhttp error " + std::to_string(code);
  if (got > 0 && wbuf) {
    out += ": " + Trim(WideToUtf8(std::wstring(wbuf, got)));
  }
  if (wbuf) {
    LocalFree(wbuf);
  }
  return out;
}

std::map<std::string, std::string> ParseRawHeaders(const std::wstring &raw) {
  std::map<std::string, std::string> out;
  std::wistringstream iss(raw);
  std::wstring line;
  while (std::getline(iss, line)) {
    if (!line.empty() && line.back() == L'\r')
      line.pop_back();
    const auto pos = line.find(L':');
    if (pos == std::wstring::npos)
      continue;
    const std::string key = ToLower(Trim(WideToUtf8(line.substr(0, pos))));
    const std::string value = Trim(WideToUtf8(line.substr(pos + 1)));
    if (!key.empty())
      out[key] = value;
  }
  return out;
}

HttpResponse Request(const std::string &method, const std::string &url,
                     const std::vector<std::string> &headers,
                     const std::optional<std::string> &body,
                     long timeout_seconds = 60) {
  HttpResponse result;
  const auto start = std::chrono::steady_clock::now();

  HINTERNET h_session = nullptr;
  HINTERNET h_connect = nullptr;
  HINTERNET h_request = nullptr;

  auto finish = [&]() -> HttpResponse {
    if (h_request)
      WinHttpCloseHandle(h_request);
    if (h_connect)
      WinHttpCloseHandle(h_connect);
    if (h_session)
      WinHttpCloseHandle(h_session);
    const auto end = std::chrono::steady_clock::now();
    result.latency_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
            .count();
    return result;
  };

  const std::wstring wurl = Utf8ToWide(url);
  URL_COMPONENTS comps{};
  comps.dwStructSize = sizeof(comps);
  comps.dwSchemeLength = static_cast<DWORD>(-1);
  comps.dwHostNameLength = static_cast<DWORD>(-1);
  comps.dwUrlPathLength = static_cast<DWORD>(-1);
  comps.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &comps)) {
    result.error = WinErrText(GetLastError());
    return finish();
  }

  const std::wstring host(comps.lpszHostName, comps.dwHostNameLength);
  std::wstring path =
      comps.dwUrlPathLength > 0
          ? std::wstring(comps.lpszUrlPath, comps.dwUrlPathLength)
          : L"/";
  if (comps.dwExtraInfoLength > 0) {
    path += std::wstring(comps.lpszExtraInfo, comps.dwExtraInfoLength);
  }
  const bool secure = comps.nScheme == INTERNET_SCHEME_HTTPS;

  h_session =
      WinHttpOpen(L"llm-audit-gui/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (!h_session) {
    result.error = WinErrText(GetLastError());
    return finish();
  }
  WinHttpSetTimeouts(h_session, timeout_seconds * 1000, timeout_seconds * 1000,
                     timeout_seconds * 1000, timeout_seconds * 1000);

  h_connect = WinHttpConnect(h_session, host.c_str(), comps.nPort, 0);
  if (!h_connect) {
    result.error = WinErrText(GetLastError());
    return finish();
  }

  const std::wstring wmethod = Utf8ToWide(method);
  h_request = WinHttpOpenRequest(
      h_connect, wmethod.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
  if (!h_request) {
    result.error = WinErrText(GetLastError());
    return finish();
  }

  for (const auto &h : headers) {
    const std::wstring wh = Utf8ToWide(h);
    if (!WinHttpAddRequestHeaders(h_request, wh.c_str(),
                                  static_cast<DWORD>(wh.size()),
                                  WINHTTP_ADDREQ_FLAG_ADD)) {
      result.error = WinErrText(GetLastError());
      return finish();
    }
  }
  const std::wstring user_agent = L"User-Agent: llm-audit-gui/1.0";
  WinHttpAddRequestHeaders(h_request, user_agent.c_str(),
                           static_cast<DWORD>(user_agent.size()),
                           WINHTTP_ADDREQ_FLAG_ADD);

  LPVOID body_ptr = WINHTTP_NO_REQUEST_DATA;
  DWORD body_len = 0;
  if (body.has_value()) {
    body_ptr = const_cast<char *>(body->data());
    body_len = static_cast<DWORD>(body->size());
  }

  if (!WinHttpSendRequest(h_request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, body_ptr,
                          body_len, body_len, 0)) {
    result.error = WinErrText(GetLastError());
    return finish();
  }
  if (!WinHttpReceiveResponse(h_request, nullptr)) {
    result.error = WinErrText(GetLastError());
    return finish();
  }

  DWORD status = 0;
  DWORD status_size = sizeof(status);
  if (!WinHttpQueryHeaders(
          h_request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
          WINHTTP_NO_HEADER_INDEX)) {
    result.error = WinErrText(GetLastError());
    return finish();
  }
  result.status = static_cast<long>(status);

  DWORD raw_size = 0;
  WinHttpQueryHeaders(h_request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                      WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &raw_size,
                      WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() == ERROR_INSUFFICIENT_BUFFER &&
      raw_size > sizeof(wchar_t)) {
    std::wstring raw(raw_size / sizeof(wchar_t), L'\0');
    if (WinHttpQueryHeaders(h_request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                            WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &raw_size,
                            WINHTTP_NO_HEADER_INDEX)) {
      result.headers = ParseRawHeaders(raw);
    }
  }

  std::string body_out;
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(h_request, &available)) {
      result.error = WinErrText(GetLastError());
      return finish();
    }
    if (available == 0)
      break;

    std::string chunk(available, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(h_request, chunk.data(), available, &read)) {
      result.error = WinErrText(GetLastError());
      return finish();
    }
    chunk.resize(read);
    body_out += chunk;
  }
  result.body = std::move(body_out);
  return finish();
}
#endif

nlohmann::json ParseJson(const std::string &s) {
  const auto j = nlohmann::json::parse(s, nullptr, false);
  if (j.is_discarded())
    return nlohmann::json::object();
  return j;
}

std::map<std::string, std::string>
RateLimitHeaders(const std::map<std::string, std::string> &headers) {
  std::map<std::string, std::string> out;
  for (const auto &[k, v] : headers) {
    if (k.find("rate") != std::string::npos ||
        k.find("limit") != std::string::npos ||
        k.find("quota") != std::string::npos ||
        k.find("retry") != std::string::npos) {
      out[k] = v;
    }
  }
  return out;
}

std::vector<std::string> ExtractModelIds(const nlohmann::json &j) {
  std::vector<std::string> out;
  auto add = [&out](const nlohmann::json &arr) {
    if (!arr.is_array())
      return;
    for (const auto &item : arr) {
      if (item.is_string()) {
        out.push_back(item.get<std::string>());
      } else if (item.is_object()) {
        if (item.contains("id") && item["id"].is_string()) {
          out.push_back(item["id"].get<std::string>());
        } else if (item.contains("name") && item["name"].is_string()) {
          out.push_back(item["name"].get<std::string>());
        }
      }
    }
  };

  if (j.is_array()) {
    add(j);
  } else if (j.is_object()) {
    if (j.contains("data"))
      add(j["data"]);
    if (j.contains("models"))
      add(j["models"]);
  }

  std::set<std::string> dedup(out.begin(), out.end());
  return {dedup.begin(), dedup.end()};
}

long long ExtractMaxContext(const nlohmann::json &j) {
  long long best = -1;
  std::function<void(const nlohmann::json &, std::string_view)> scan =
      [&](const nlohmann::json &node, std::string_view key_name) {
        if (node.is_object()) {
          for (const auto &[k, v] : node.items()) {
            const std::string k_low = ToLower(k);
            if (v.is_number_integer()) {
              const auto n = v.get<long long>();
              const bool candidate =
                  k_low.find("context") != std::string::npos ||
                  k_low.find("inputtokenlimit") != std::string::npos ||
                  k_low.find("contextwindow") != std::string::npos ||
                  (k_low.find("token") != std::string::npos &&
                   k_low.find("output") == std::string::npos &&
                   k_low.find("completion") == std::string::npos);
              if (candidate) {
                best = std::max(best, n);
              }
            }
            scan(v, k_low);
          }
        } else if (node.is_array()) {
          for (const auto &item : node)
            scan(item, key_name);
        }
      };

  scan(j, "");
  return best;
}

std::vector<std::string>
ExtractCapabilities(const std::vector<std::string> &model_ids,
                    const nlohmann::json &raw) {
  std::set<std::string> caps;
  auto consider = [&caps](const std::string &text) {
    const auto t = ToLower(text);
    if (t.find("vision") != std::string::npos ||
        t.find("vl") != std::string::npos ||
        t.find("image") != std::string::npos) {
      caps.insert("vision/image");
    }
    if (t.find("reason") != std::string::npos ||
        t.find("thinking") != std::string::npos) {
      caps.insert("reasoning");
    }
    if (t.find("coder") != std::string::npos ||
        t.find("code") != std::string::npos) {
      caps.insert("coding");
    }
    if (t.find("embed") != std::string::npos) {
      caps.insert("embeddings");
    }
    if (t.find("audio") != std::string::npos ||
        t.find("speech") != std::string::npos) {
      caps.insert("audio");
    }
    if (t.find("rerank") != std::string::npos) {
      caps.insert("reranking");
    }
    if (t.find("tool") != std::string::npos ||
        t.find("function") != std::string::npos) {
      caps.insert("tool_use");
    }
  };

  for (const auto &id : model_ids)
    consider(id);

  std::function<void(const nlohmann::json &)> walk =
      [&](const nlohmann::json &node) {
        if (node.is_object()) {
          for (const auto &[k, v] : node.items()) {
            consider(k);
            if (v.is_string())
              consider(v.get<std::string>());
            walk(v);
          }
        } else if (node.is_array()) {
          for (const auto &it : node)
            walk(it);
        }
      };
  walk(raw);

  return {caps.begin(), caps.end()};
}

std::string ChooseModel(const std::vector<std::string> &discovered,
                        const std::vector<std::string> &preferred) {
  if (discovered.empty())
    return {};
  std::vector<std::string> lower;
  lower.reserve(discovered.size());
  for (const auto &d : discovered)
    lower.push_back(ToLower(d));

  for (const auto &pref : preferred) {
    const auto p = ToLower(pref);
    for (std::size_t i = 0; i < discovered.size(); ++i) {
      if (lower[i] == p || lower[i].find(p) != std::string::npos) {
        return discovered[i];
      }
    }
  }
  return discovered.front();
}

std::string ExtractOpenAIText(const nlohmann::json &j) {
  if (!j.is_object() || !j.contains("choices") || !j["choices"].is_array() ||
      j["choices"].empty()) {
    return {};
  }
  const auto &choice = j["choices"][0];
  if (!choice.is_object() || !choice.contains("message"))
    return {};
  const auto &msg = choice["message"];
  if (!msg.is_object() || !msg.contains("content"))
    return {};
  const auto &content = msg["content"];
  if (content.is_string())
    return content.get<std::string>();
  if (content.is_array()) {
    std::ostringstream oss;
    for (const auto &part : content) {
      if (part.is_object() && part.contains("text") &&
          part["text"].is_string()) {
        oss << part["text"].get<std::string>() << "\n";
      }
    }
    return oss.str();
  }
  return {};
}

std::string ExtractGoogleText(const nlohmann::json &j) {
  if (!j.is_object() || !j.contains("candidates") ||
      !j["candidates"].is_array() || j["candidates"].empty()) {
    return {};
  }
  const auto &c0 = j["candidates"][0];
  if (!c0.is_object() || !c0.contains("content"))
    return {};
  const auto &content = c0["content"];
  if (!content.is_object() || !content.contains("parts") ||
      !content["parts"].is_array())
    return {};
  std::ostringstream oss;
  for (const auto &part : content["parts"]) {
    if (part.is_object() && part.contains("text") && part["text"].is_string()) {
      oss << part["text"].get<std::string>() << "\n";
    }
  }
  return oss.str();
}

std::string ExtractCohereText(const nlohmann::json &j) {
  if (!j.is_object())
    return {};
  if (j.contains("text") && j["text"].is_string())
    return j["text"].get<std::string>();
  if (j.contains("message") && j["message"].is_object()) {
    const auto &m = j["message"];
    if (m.contains("content") && m["content"].is_array()) {
      std::ostringstream oss;
      for (const auto &part : m["content"]) {
        if (part.is_object() && part.contains("text") &&
            part["text"].is_string()) {
          oss << part["text"].get<std::string>() << "\n";
        }
      }
      return oss.str();
    }
  }
  return {};
}

void ScoreProvider(ProviderAudit &p) {
  auto get_answer = [&](const std::string &name) {
    for (const auto &t : p.prompt_tests) {
      if (t.name == name)
        return ToLower(t.answer);
    }
    return std::string{};
  };

  const auto reasoning = get_answer("reasoning");
  const auto coding = get_answer("coding");
  const auto axui = get_answer("axui");

  p.score_reasoning =
      (reasoning.find("5 minute") != std::string::npos || reasoning == "5" ||
       reasoning.find("five minutes") != std::string::npos)
          ? 1
          : 0;
  p.score_coding = (coding.find("[::-1]") != std::string::npos ||
                    coding.find("reversed(") != std::string::npos)
                       ? 1
                       : 0;

  const bool has_close = axui.find("close_popup") != std::string::npos;
  const bool has_continue = axui.find("btn_continue") != std::string::npos;
  if (has_close && has_continue) {
    p.score_axui = 2;
  } else if (has_close || has_continue) {
    p.score_axui = 1;
  } else {
    p.score_axui = 0;
  }

  p.score_total = p.score_reasoning + p.score_coding + p.score_axui;
}

void AddTrace(ProviderAudit &p, const std::string &step,
              const std::string &method, const std::string &url,
              const HttpResponse &r) {
  RequestTrace t;
  t.step = step;
  t.method = method;
  t.url = url;
  t.status = r.status;
  t.latency_ms = r.latency_ms;
  t.rate_limit_headers = RateLimitHeaders(r.headers);
  t.response_snippet = Snippet(r.body);
  t.error = r.error;
  p.request_traces.push_back(std::move(t));

  p.total_requests += 1;
  if (r.status >= 200 && r.status < 300 && r.error.empty()) {
    p.successful_requests += 1;
  } else {
    p.failed_requests += 1;
  }
}

void FinalizeMetrics(ProviderAudit &p) {
  std::vector<long> lats;
  for (const auto &t : p.request_traces) {
    if (t.latency_ms >= 0)
      lats.push_back(t.latency_ms);
  }
  if (!lats.empty()) {
    const auto sum = std::accumulate(lats.begin(), lats.end(), 0LL);
    p.avg_latency_ms =
        static_cast<long>(sum / static_cast<long long>(lats.size()));
  }
}

std::vector<std::string>
TopCandidates(const std::vector<std::string> &discovered,
              const std::vector<std::string> &preferred,
              std::size_t max_count) {
  std::vector<std::string> out;
  std::set<std::string> used;

  for (const auto &pref : preferred) {
    const auto p = ToLower(pref);
    for (const auto &d : discovered) {
      const auto dl = ToLower(d);
      if ((dl == p || dl.find(p) != std::string::npos) && !used.count(d)) {
        out.push_back(d);
        used.insert(d);
      }
      if (out.size() >= max_count)
        return out;
    }
  }

  for (const auto &d : discovered) {
    if (!used.count(d)) {
      out.push_back(d);
      used.insert(d);
      if (out.size() >= max_count)
        return out;
    }
  }
  return out;
}

ProviderAudit
AuditOpenAICompatible(const std::string &provider_id,
                      const std::string &provider_name, const std::string &key,
                      const std::string &list_url, const std::string &chat_url,
                      const std::vector<std::string> &preferred_models,
                      const std::vector<std::string> &extra_headers,
                      const LogFn &log,
                      const std::atomic<bool> &cancel_requested) {
  ProviderAudit p;
  p.provider_id = provider_id;
  p.provider_name = provider_name;
  p.api_key = key;
  p.key_supplied = !key.empty();

  if (key.empty()) {
    p.notes = "No API key supplied.";
    return p;
  }

  std::vector<std::string> base_headers = extra_headers;
  base_headers.push_back("Authorization: Bearer " + key);

  log("[" + provider_name + "] Fetching model list");
  const auto list_resp = Request("GET", list_url, base_headers, std::nullopt);
  AddTrace(p, "list_models", "GET", list_url, list_resp);

  p.models_status = list_resp.status;
  p.models_latency_ms = list_resp.latency_ms;
  p.models_rate_limit_headers = RateLimitHeaders(list_resp.headers);
  if (list_resp.status < 200 || list_resp.status >= 300) {
    p.error_snippet = Snippet(list_resp.body);
  }

  const auto models_json = ParseJson(list_resp.body);
  p.raw_payload["models_response"] = models_json;
  const auto discovered = ExtractModelIds(models_json);
  p.sample_models.assign(
      discovered.begin(),
      discovered.begin() +
          static_cast<long>(std::min<std::size_t>(discovered.size(), 30)));
  p.max_context_seen = ExtractMaxContext(models_json);
  p.capability_tags = ExtractCapabilities(discovered, models_json);

  if (discovered.empty()) {
    p.notes = "No models discovered or access denied.";
    FinalizeMetrics(p);
    return p;
  }

  const auto model_candidates = TopCandidates(discovered, preferred_models, 8);
  for (const auto &model : model_candidates) {
    if (cancel_requested.load()) {
      p.notes += " Audit canceled by user.";
      FinalizeMetrics(p);
      return p;
    }

    nlohmann::json payload = {
        {"model", model},
        {"messages",
         {{{"role", "user"}, {"content", "Reply with exactly: OK"}}}},
        {"temperature", 0},
        {"max_tokens", 64},
    };

    auto headers = base_headers;
    headers.push_back("Content-Type: application/json");
    const auto chat_resp = Request("POST", chat_url, headers, payload.dump());
    AddTrace(p, "model_check:" + model, "POST", chat_url, chat_resp);

    ModelCheck mc;
    mc.model = model;
    mc.status = chat_resp.status;
    mc.latency_ms = chat_resp.latency_ms;
    mc.error_snippet = Snippet(chat_resp.body);
    const auto parsed = ParseJson(chat_resp.body);
    const auto answer = ExtractOpenAIText(parsed);
    mc.working =
        (chat_resp.status >= 200 && chat_resp.status < 300 && !answer.empty());
    p.model_checks.push_back(mc);
    if (mc.working) {
      p.working_models.push_back(model);
    } else {
      p.failing_models.push_back(model);
    }
  }

  p.model_used = !p.working_models.empty()
                     ? p.working_models.front()
                     : ChooseModel(discovered, preferred_models);

  for (const auto &prompt : kPromptSuite) {
    if (cancel_requested.load()) {
      p.notes += " Audit canceled by user.";
      FinalizeMetrics(p);
      return p;
    }

    log("[" + provider_name + "] Prompt test: " + prompt.first);
    nlohmann::json payload = {
        {"model", p.model_used},
        {"messages", {{{"role", "user"}, {"content", prompt.second}}}},
        {"temperature", 0},
        {"max_tokens", 300},
    };

    auto headers = base_headers;
    headers.push_back("Content-Type: application/json");
    const auto resp = Request("POST", chat_url, headers, payload.dump());
    AddTrace(p, "prompt_test:" + prompt.first, "POST", chat_url, resp);

    const auto parsed = ParseJson(resp.body);
    PromptTest t;
    t.name = prompt.first;
    t.status = resp.status;
    t.latency_ms = resp.latency_ms;
    t.rate_limit_headers = RateLimitHeaders(resp.headers);
    t.answer = Snippet(ExtractOpenAIText(parsed), 1400);
    if (resp.status < 200 || resp.status >= 300) {
      t.error_snippet = Snippet(resp.body, 700);
    }
    p.prompt_tests.push_back(std::move(t));
  }

  ScoreProvider(p);
  FinalizeMetrics(p);
  return p;
}

ProviderAudit AuditGoogle(const std::string &key, const LogFn &log,
                          const std::atomic<bool> &cancel_requested) {
  ProviderAudit p;
  p.provider_id = "google_ai_studio";
  p.provider_name = "Google AI Studio";
  p.api_key = key;
  p.key_supplied = !key.empty();

  if (key.empty()) {
    p.notes = "No API key supplied.";
    return p;
  }

  const std::string list_url =
      "https://generativelanguage.googleapis.com/v1beta/models?key=" + key;
  log("[Google AI Studio] Fetching model list");
  const auto list_resp = Request("GET", list_url, {}, std::nullopt);
  AddTrace(p, "list_models", "GET", list_url, list_resp);
  p.models_status = list_resp.status;
  p.models_latency_ms = list_resp.latency_ms;
  p.models_rate_limit_headers = RateLimitHeaders(list_resp.headers);
  if (list_resp.status < 200 || list_resp.status >= 300) {
    p.error_snippet = Snippet(list_resp.body);
  }

  const auto list_json = ParseJson(list_resp.body);
  p.raw_payload["models_response"] = list_json;

  std::vector<std::string> discovered;
  if (list_json.is_object() && list_json.contains("models") &&
      list_json["models"].is_array()) {
    for (const auto &model : list_json["models"]) {
      if (!model.is_object())
        continue;
      if (!model.contains("name") || !model["name"].is_string())
        continue;
      const std::string name = model["name"].get<std::string>();
      bool supports_generate = false;
      if (model.contains("supportedGenerationMethods") &&
          model["supportedGenerationMethods"].is_array()) {
        for (const auto &m : model["supportedGenerationMethods"]) {
          if (m.is_string() && m.get<std::string>() == "generateContent") {
            supports_generate = true;
            break;
          }
        }
      }
      if (supports_generate)
        discovered.push_back(name);
    }
  }

  std::set<std::string> dedup(discovered.begin(), discovered.end());
  discovered.assign(dedup.begin(), dedup.end());
  p.sample_models.assign(
      discovered.begin(),
      discovered.begin() +
          static_cast<long>(std::min<std::size_t>(discovered.size(), 30)));
  p.max_context_seen = ExtractMaxContext(list_json);
  p.capability_tags = ExtractCapabilities(discovered, list_json);

  const std::vector<std::string> preferred = {
      "models/gemini-2.5-pro", "models/gemini-2.5-flash",
      "models/gemini-2.0-flash", "models/gemini-1.5-pro"};

  if (discovered.empty()) {
    p.notes = "No generateContent models discovered.";
    FinalizeMetrics(p);
    return p;
  }

  const auto check_candidates = TopCandidates(discovered, preferred, 8);
  for (const auto &model : check_candidates) {
    if (cancel_requested.load()) {
      p.notes += " Audit canceled by user.";
      FinalizeMetrics(p);
      return p;
    }
    nlohmann::json payload = {
        {"contents",
         {{{"role", "user"},
           {"parts", {{{"text", "Reply with exactly: OK"}}}}}}},
        {"generationConfig", {{"temperature", 0}, {"maxOutputTokens", 64}}},
    };

    const std::string url =
        "https://generativelanguage.googleapis.com/v1beta/" + model +
        ":generateContent?key=" + key;
    const auto resp = Request("POST", url, {"Content-Type: application/json"},
                              payload.dump());
    AddTrace(p, "model_check:" + model, "POST", url, resp);

    ModelCheck mc;
    mc.model = model;
    mc.status = resp.status;
    mc.latency_ms = resp.latency_ms;
    mc.error_snippet = Snippet(resp.body);
    const auto parsed = ParseJson(resp.body);
    const auto answer = ExtractGoogleText(parsed);
    mc.working = (resp.status >= 200 && resp.status < 300 && !answer.empty());
    p.model_checks.push_back(mc);
    if (mc.working)
      p.working_models.push_back(model);
    if (!mc.working)
      p.failing_models.push_back(model);
  }

  p.model_used = !p.working_models.empty() ? p.working_models.front()
                                           : ChooseModel(discovered, preferred);

  for (const auto &prompt : kPromptSuite) {
    if (cancel_requested.load()) {
      p.notes += " Audit canceled by user.";
      FinalizeMetrics(p);
      return p;
    }
    log("[Google AI Studio] Prompt test: " + prompt.first);

    nlohmann::json payload = {
        {"contents",
         {{{"role", "user"}, {"parts", {{{"text", prompt.second}}}}}}},
        {"generationConfig", {{"temperature", 0}, {"maxOutputTokens", 300}}},
    };

    const std::string url =
        "https://generativelanguage.googleapis.com/v1beta/" + p.model_used +
        ":generateContent?key=" + key;
    const auto resp = Request("POST", url, {"Content-Type: application/json"},
                              payload.dump());
    AddTrace(p, "prompt_test:" + prompt.first, "POST", url, resp);

    PromptTest t;
    t.name = prompt.first;
    t.status = resp.status;
    t.latency_ms = resp.latency_ms;
    t.rate_limit_headers = RateLimitHeaders(resp.headers);
    t.answer = Snippet(ExtractGoogleText(ParseJson(resp.body)), 1400);
    if (resp.status < 200 || resp.status >= 300)
      t.error_snippet = Snippet(resp.body, 700);
    p.prompt_tests.push_back(std::move(t));
  }

  ScoreProvider(p);
  FinalizeMetrics(p);
  return p;
}

ProviderAudit AuditCohere(const std::string &key, const LogFn &log,
                          const std::atomic<bool> &cancel_requested) {
  ProviderAudit p;
  p.provider_id = "cohere";
  p.provider_name = "Cohere";
  p.api_key = key;
  p.key_supplied = !key.empty();

  if (key.empty()) {
    p.notes = "No API key supplied.";
    return p;
  }

  std::vector<std::string> base_headers = {
      "Authorization: Bearer " + key,
      "Cohere-Version: 2022-12-06",
  };

  const std::string list_url = "https://api.cohere.com/v1/models";
  log("[Cohere] Fetching model list");
  const auto list_resp = Request("GET", list_url, base_headers, std::nullopt);
  AddTrace(p, "list_models", "GET", list_url, list_resp);

  p.models_status = list_resp.status;
  p.models_latency_ms = list_resp.latency_ms;
  p.models_rate_limit_headers = RateLimitHeaders(list_resp.headers);
  if (list_resp.status < 200 || list_resp.status >= 300)
    p.error_snippet = Snippet(list_resp.body);

  const auto list_json = ParseJson(list_resp.body);
  p.raw_payload["models_response"] = list_json;

  std::vector<std::string> discovered;
  if (list_json.is_object() && list_json.contains("models") &&
      list_json["models"].is_array()) {
    for (const auto &model : list_json["models"]) {
      if (model.is_object() && model.contains("name") &&
          model["name"].is_string()) {
        discovered.push_back(model["name"].get<std::string>());
      } else if (model.is_string()) {
        discovered.push_back(model.get<std::string>());
      }
    }
  }

  std::set<std::string> dedup(discovered.begin(), discovered.end());
  discovered.assign(dedup.begin(), dedup.end());
  p.sample_models.assign(
      discovered.begin(),
      discovered.begin() +
          static_cast<long>(std::min<std::size_t>(discovered.size(), 30)));
  p.max_context_seen = ExtractMaxContext(list_json);
  p.capability_tags = ExtractCapabilities(discovered, list_json);

  const std::vector<std::string> preferred = {"command-a-reasoning-08-2025",
                                              "command-r-08-2024",
                                              "command-a-vision-07-2025"};

  std::vector<std::string> chat_candidates;
  for (const auto &m : discovered) {
    const auto ml = ToLower(m);
    if (ml.find("embed") != std::string::npos ||
        ml.find("rerank") != std::string::npos)
      continue;
    chat_candidates.push_back(m);
  }

  if (chat_candidates.empty()) {
    p.notes = "No chat-capable models inferred from model names.";
    FinalizeMetrics(p);
    return p;
  }

  const auto checks = TopCandidates(chat_candidates, preferred, 8);
  for (const auto &model : checks) {
    if (cancel_requested.load()) {
      p.notes += " Audit canceled by user.";
      FinalizeMetrics(p);
      return p;
    }

    nlohmann::json payload = {
        {"model", model},
        {"message", "Reply with exactly: OK"},
        {"temperature", 0},
        {"max_tokens", 64},
    };

    auto headers = base_headers;
    headers.push_back("Content-Type: application/json");
    const auto resp = Request("POST", "https://api.cohere.com/v1/chat", headers,
                              payload.dump());
    AddTrace(p, "model_check:" + model, "POST",
             "https://api.cohere.com/v1/chat", resp);

    ModelCheck mc;
    mc.model = model;
    mc.status = resp.status;
    mc.latency_ms = resp.latency_ms;
    mc.error_snippet = Snippet(resp.body);
    mc.working = (resp.status >= 200 && resp.status < 300 &&
                  !ExtractCohereText(ParseJson(resp.body)).empty());
    p.model_checks.push_back(mc);
    if (mc.working)
      p.working_models.push_back(model);
    if (!mc.working)
      p.failing_models.push_back(model);
  }

  p.model_used = !p.working_models.empty()
                     ? p.working_models.front()
                     : ChooseModel(chat_candidates, preferred);

  for (const auto &prompt : kPromptSuite) {
    if (cancel_requested.load()) {
      p.notes += " Audit canceled by user.";
      FinalizeMetrics(p);
      return p;
    }

    log("[Cohere] Prompt test: " + prompt.first);
    nlohmann::json payload = {
        {"model", p.model_used},
        {"message", prompt.second},
        {"temperature", 0},
        {"max_tokens", 300},
    };

    auto headers = base_headers;
    headers.push_back("Content-Type: application/json");
    const auto resp = Request("POST", "https://api.cohere.com/v1/chat", headers,
                              payload.dump());
    AddTrace(p, "prompt_test:" + prompt.first, "POST",
             "https://api.cohere.com/v1/chat", resp);

    PromptTest t;
    t.name = prompt.first;
    t.status = resp.status;
    t.latency_ms = resp.latency_ms;
    t.rate_limit_headers = RateLimitHeaders(resp.headers);
    t.answer = Snippet(ExtractCohereText(ParseJson(resp.body)), 1400);
    if (resp.status < 200 || resp.status >= 300)
      t.error_snippet = Snippet(resp.body, 700);
    p.prompt_tests.push_back(std::move(t));
  }

  ScoreProvider(p);
  FinalizeMetrics(p);
  return p;
}

ProviderAudit AuditVercel(const std::string &key, const LogFn &log,
                          const std::atomic<bool> &cancel_requested) {
  ProviderAudit p;
  p.provider_id = "vercel";
  p.provider_name = "Vercel AI Gateway";
  p.api_key = key;
  p.key_supplied = !key.empty();

  if (key.empty()) {
    p.notes = "No API key supplied.";
    return p;
  }

  const std::vector<std::string> headers = {"Authorization: Bearer " + key};

  log("[Vercel] Validating user token");
  const auto auth_resp =
      Request("GET", "https://api.vercel.com/v2/user", headers, std::nullopt);
  AddTrace(p, "auth_user", "GET", "https://api.vercel.com/v2/user", auth_resp);
  p.auth_status = auth_resp.status;
  p.auth_latency_ms = auth_resp.latency_ms;
  p.auth_rate_limit_headers = RateLimitHeaders(auth_resp.headers);

  log("[Vercel] Fetching AI Gateway models");
  const auto list_resp = Request(
      "GET", "https://ai-gateway.vercel.sh/v1/models", headers, std::nullopt);
  AddTrace(p, "list_models", "GET", "https://ai-gateway.vercel.sh/v1/models",
           list_resp);
  p.models_status = list_resp.status;
  p.models_latency_ms = list_resp.latency_ms;
  p.models_rate_limit_headers = RateLimitHeaders(list_resp.headers);
  if (list_resp.status < 200 || list_resp.status >= 300)
    p.error_snippet = Snippet(list_resp.body);

  const auto list_json = ParseJson(list_resp.body);
  p.raw_payload["auth_response"] = ParseJson(auth_resp.body);
  p.raw_payload["models_response"] = list_json;

  auto discovered = ExtractModelIds(list_json);
  p.sample_models.assign(
      discovered.begin(),
      discovered.begin() +
          static_cast<long>(std::min<std::size_t>(discovered.size(), 30)));
  p.max_context_seen = ExtractMaxContext(list_json);
  p.capability_tags = ExtractCapabilities(discovered, list_json);

  const std::vector<std::string> preferred = {
      "openai/gpt-5", "openai/gpt-4.1", "openai/gpt-4o",
      "anthropic/claude-3.7-sonnet", "google/gemini-2.5-pro"};

  if (discovered.empty()) {
    p.notes = "No models discovered from AI Gateway.";
    FinalizeMetrics(p);
    return p;
  }

  const auto checks = TopCandidates(discovered, preferred, 8);
  std::vector<std::string> post_headers = headers;
  post_headers.push_back("Content-Type: application/json");

  for (const auto &model : checks) {
    if (cancel_requested.load()) {
      p.notes += " Audit canceled by user.";
      FinalizeMetrics(p);
      return p;
    }
    nlohmann::json payload = {
        {"model", model},
        {"messages",
         {{{"role", "user"}, {"content", "Reply with exactly: OK"}}}},
        {"max_tokens", 64},
    };
    const auto resp =
        Request("POST", "https://ai-gateway.vercel.sh/v1/chat/completions",
                post_headers, payload.dump());
    AddTrace(p, "model_check:" + model, "POST",
             "https://ai-gateway.vercel.sh/v1/chat/completions", resp);

    ModelCheck mc;
    mc.model = model;
    mc.status = resp.status;
    mc.latency_ms = resp.latency_ms;
    mc.error_snippet = Snippet(resp.body);
    mc.working = (resp.status >= 200 && resp.status < 300 &&
                  !ExtractOpenAIText(ParseJson(resp.body)).empty());
    p.model_checks.push_back(mc);
    if (mc.working)
      p.working_models.push_back(model);
    if (!mc.working)
      p.failing_models.push_back(model);
  }

  p.model_used = !p.working_models.empty() ? p.working_models.front()
                                           : ChooseModel(discovered, preferred);

  for (const auto &prompt : kPromptSuite) {
    if (cancel_requested.load()) {
      p.notes += " Audit canceled by user.";
      FinalizeMetrics(p);
      return p;
    }

    log("[Vercel] Prompt test: " + prompt.first);
    nlohmann::json payload = {
        {"model", p.model_used},
        {"messages", {{{"role", "user"}, {"content", prompt.second}}}},
        {"max_tokens", 300},
    };
    const auto resp =
        Request("POST", "https://ai-gateway.vercel.sh/v1/chat/completions",
                post_headers, payload.dump());
    AddTrace(p, "prompt_test:" + prompt.first, "POST",
             "https://ai-gateway.vercel.sh/v1/chat/completions", resp);

    PromptTest t;
    t.name = prompt.first;
    t.status = resp.status;
    t.latency_ms = resp.latency_ms;
    t.rate_limit_headers = RateLimitHeaders(resp.headers);
    t.answer = Snippet(ExtractOpenAIText(ParseJson(resp.body)), 1400);
    if (resp.status < 200 || resp.status >= 300)
      t.error_snippet = Snippet(resp.body, 700);
    p.prompt_tests.push_back(std::move(t));
  }

  ScoreProvider(p);
  FinalizeMetrics(p);
  return p;
}

ProviderAudit AuditGitHubToken(const std::string &provider_id,
                               const std::string &provider_name,
                               const std::string &token, const LogFn &log,
                               const std::atomic<bool> &cancel_requested) {
  ProviderAudit p;
  p.provider_id = provider_id;
  p.provider_name = provider_name;
  p.api_key = token;
  p.key_supplied = !token.empty();

  if (token.empty()) {
    p.notes = "No API key supplied.";
    return p;
  }

  const std::vector<std::string> gh_headers = {
      "Authorization: Bearer " + token,
      "Accept: application/vnd.github+json",
  };

  log("[" + provider_name + "] Validating GitHub user scope");
  const auto user_resp =
      Request("GET", "https://api.github.com/user", gh_headers, std::nullopt);
  AddTrace(p, "auth_user", "GET", "https://api.github.com/user", user_resp);
  p.auth_status = user_resp.status;
  p.auth_latency_ms = user_resp.latency_ms;
  p.auth_rate_limit_headers = RateLimitHeaders(user_resp.headers);

  const std::vector<std::string> models_headers = {
      "Authorization: Bearer " + token,
  };
  log("[" + provider_name + "] Fetching GitHub Models catalog");
  const auto list_resp =
      Request("GET", "https://models.inference.ai.azure.com/models",
              models_headers, std::nullopt);
  AddTrace(p, "list_models", "GET",
           "https://models.inference.ai.azure.com/models", list_resp);

  p.models_status = list_resp.status;
  p.models_latency_ms = list_resp.latency_ms;
  p.models_rate_limit_headers = RateLimitHeaders(list_resp.headers);
  if (list_resp.status < 200 || list_resp.status >= 300)
    p.error_snippet = Snippet(list_resp.body);

  const auto list_json = ParseJson(list_resp.body);
  p.raw_payload["user_response"] = ParseJson(user_resp.body);
  p.raw_payload["models_response"] = list_json;

  auto discovered = ExtractModelIds(list_json);
  p.sample_models.assign(
      discovered.begin(),
      discovered.begin() +
          static_cast<long>(std::min<std::size_t>(discovered.size(), 30)));
  p.max_context_seen = ExtractMaxContext(list_json);
  p.capability_tags = ExtractCapabilities(discovered, list_json);

  const std::vector<std::string> preferred = {
      "gpt-4.1", "gpt-4o", "gpt-4o-mini", "deepseek-r1", "phi-4"};

  if (discovered.empty()) {
    p.notes = "No models discovered for this token.";
    FinalizeMetrics(p);
    return p;
  }

  const auto checks = TopCandidates(discovered, preferred, 8);
  std::vector<std::string> post_headers = models_headers;
  post_headers.push_back("Content-Type: application/json");

  for (const auto &model : checks) {
    if (cancel_requested.load()) {
      p.notes += " Audit canceled by user.";
      FinalizeMetrics(p);
      return p;
    }

    nlohmann::json payload = {
        {"model", model},
        {"messages",
         {{{"role", "user"}, {"content", "Reply with exactly: OK"}}}},
        {"temperature", 0},
        {"max_tokens", 64},
    };

    const auto resp = Request(
        "POST", "https://models.inference.ai.azure.com/chat/completions",
        post_headers, payload.dump());
    AddTrace(p, "model_check:" + model, "POST",
             "https://models.inference.ai.azure.com/chat/completions", resp);

    ModelCheck mc;
    mc.model = model;
    mc.status = resp.status;
    mc.latency_ms = resp.latency_ms;
    mc.error_snippet = Snippet(resp.body);
    mc.working = (resp.status >= 200 && resp.status < 300 &&
                  !ExtractOpenAIText(ParseJson(resp.body)).empty());
    p.model_checks.push_back(mc);
    if (mc.working)
      p.working_models.push_back(model);
    if (!mc.working)
      p.failing_models.push_back(model);
  }

  p.model_used = !p.working_models.empty() ? p.working_models.front()
                                           : ChooseModel(discovered, preferred);

  for (const auto &prompt : kPromptSuite) {
    if (cancel_requested.load()) {
      p.notes += " Audit canceled by user.";
      FinalizeMetrics(p);
      return p;
    }

    log("[" + provider_name + "] Prompt test: " + prompt.first);
    nlohmann::json payload = {
        {"model", p.model_used},
        {"messages", {{{"role", "user"}, {"content", prompt.second}}}},
        {"temperature", 0},
        {"max_tokens", 300},
    };

    const auto resp = Request(
        "POST", "https://models.inference.ai.azure.com/chat/completions",
        post_headers, payload.dump());
    AddTrace(p, "prompt_test:" + prompt.first, "POST",
             "https://models.inference.ai.azure.com/chat/completions", resp);

    PromptTest t;
    t.name = prompt.first;
    t.status = resp.status;
    t.latency_ms = resp.latency_ms;
    t.rate_limit_headers = RateLimitHeaders(resp.headers);
    t.answer = Snippet(ExtractOpenAIText(ParseJson(resp.body)), 1400);
    if (resp.status < 200 || resp.status >= 300)
      t.error_snippet = Snippet(resp.body, 700);
    p.prompt_tests.push_back(std::move(t));
  }

  ScoreProvider(p);
  FinalizeMetrics(p);
  return p;
}

} // namespace

AuditReport AuditEngine::Run(const std::map<std::string, std::string> &keys,
                             const LogFn &log,
                             const std::atomic<bool> &cancel_requested) {
  AuditReport report;
  report.generated_at_utc = NowUtc();

#if !defined(_WIN32)
  curl_global_init(CURL_GLOBAL_DEFAULT);
#endif

  auto push_log = [&](const std::string &message) {
    const std::string line = "[" + NowUtc() + "] " + message;
    report.run_logs.push_back(line);
    if (log)
      log(line);
  };

  auto key_of = [&](const std::string &k) {
    const auto it = keys.find(k);
    if (it == keys.end())
      return std::string{};
    return it->second;
  };

  if (cancel_requested.load()) {
    push_log("Audit canceled before start.");
#if !defined(_WIN32)
    curl_global_cleanup();
#endif
    return report;
  }

  push_log("Starting full provider audit");

  report.providers.push_back(AuditOpenAICompatible(
      "openrouter", "OpenRouter", key_of("openrouter"),
      "https://openrouter.ai/api/v1/models",
      "https://openrouter.ai/api/v1/chat/completions",
      {"openai/gpt-4.1", "openai/gpt-4o", "anthropic/claude-3.7-sonnet",
       "google/gemini-2.5-pro"},
      {}, push_log, cancel_requested));

  if (!cancel_requested.load()) {
    report.providers.push_back(
        AuditGoogle(key_of("google_ai_studio"), push_log, cancel_requested));
  }
  if (!cancel_requested.load()) {
    report.providers.push_back(AuditOpenAICompatible(
        "mistral", "Mistral", key_of("mistral"),
        "https://api.mistral.ai/v1/models",
        "https://api.mistral.ai/v1/chat/completions",
        {"mistral-large-latest", "magistral-medium-latest",
         "mistral-medium-latest", "mistral-small-latest"},
        {}, push_log, cancel_requested));
  }
  if (!cancel_requested.load()) {
    report.providers.push_back(
        AuditVercel(key_of("vercel"), push_log, cancel_requested));
  }
  if (!cancel_requested.load()) {
    report.providers.push_back(AuditOpenAICompatible(
        "groq", "Groq", key_of("groq"), "https://api.groq.com/openai/v1/models",
        "https://api.groq.com/openai/v1/chat/completions",
        {"llama-3.3-70b-versatile", "deepseek-r1-distill-llama-70b",
         "qwen/qwen3-32b"},
        {}, push_log, cancel_requested));
  }
  if (!cancel_requested.load()) {
    report.providers.push_back(
        AuditCohere(key_of("cohere"), push_log, cancel_requested));
  }
  if (!cancel_requested.load()) {
    report.providers.push_back(AuditOpenAICompatible(
        "ai21", "AI21", key_of("ai21"), "https://api.ai21.com/studio/v1/models",
        "https://api.ai21.com/studio/v1/chat/completions",
        {"jamba-1.5-large", "jamba-large", "jamba-1.5-mini", "jamba-mini"}, {},
        push_log, cancel_requested));
  }
  if (!cancel_requested.load()) {
    report.providers.push_back(
        AuditGitHubToken("github_chatgpt", "GitHub PAT (chatgpt)",
                         key_of("github_chatgpt"), push_log, cancel_requested));
  }
  if (!cancel_requested.load()) {
    report.providers.push_back(AuditGitHubToken(
        "github_chatgpt5", "GitHub PAT (chatgpt5)", key_of("github_chatgpt5"),
        push_log, cancel_requested));
  }
  if (!cancel_requested.load()) {
    report.providers.push_back(AuditGitHubToken(
        "github_deepseek", "GitHub PAT (deepseek)", key_of("github_deepseek"),
        push_log, cancel_requested));
  }
  if (!cancel_requested.load()) {
    report.providers.push_back(
        AuditGitHubToken("github_jamba", "GitHub PAT (jamba)",
                         key_of("github_jamba"), push_log, cancel_requested));
  }

  if (cancel_requested.load()) {
    push_log("Audit ended early due to cancellation request.");
  } else {
    push_log("Audit completed.");
  }

#if !defined(_WIN32)
  curl_global_cleanup();
#endif
  return report;
}

nlohmann::json ReportToJson(const AuditReport &report) {
  nlohmann::json out;
  out["generated_at_utc"] = report.generated_at_utc;
  out["run_logs"] = report.run_logs;
  out["providers"] = nlohmann::json::array();

  for (const auto &p : report.providers) {
    nlohmann::json pj;
    pj["provider_id"] = p.provider_id;
    pj["provider_name"] = p.provider_name;
    pj["api_key"] = p.api_key;
    pj["key_supplied"] = p.key_supplied;
    pj["auth_status"] = p.auth_status;
    pj["models_status"] = p.models_status;
    pj["auth_latency_ms"] = p.auth_latency_ms;
    pj["models_latency_ms"] = p.models_latency_ms;
    pj["auth_rate_limit_headers"] = p.auth_rate_limit_headers;
    pj["models_rate_limit_headers"] = p.models_rate_limit_headers;
    pj["sample_models"] = p.sample_models;
    pj["capability_tags"] = p.capability_tags;
    pj["working_models"] = p.working_models;
    pj["failing_models"] = p.failing_models;
    pj["model_used"] = p.model_used;
    pj["max_context_seen"] = p.max_context_seen;
    pj["score_reasoning"] = p.score_reasoning;
    pj["score_coding"] = p.score_coding;
    pj["score_axui"] = p.score_axui;
    pj["score_total"] = p.score_total;
    pj["total_requests"] = p.total_requests;
    pj["successful_requests"] = p.successful_requests;
    pj["failed_requests"] = p.failed_requests;
    pj["avg_latency_ms"] = p.avg_latency_ms;
    pj["notes"] = p.notes;
    pj["error_snippet"] = p.error_snippet;

    pj["model_checks"] = nlohmann::json::array();
    for (const auto &c : p.model_checks) {
      pj["model_checks"].push_back({
          {"model", c.model},
          {"status", c.status},
          {"latency_ms", c.latency_ms},
          {"working", c.working},
          {"error_snippet", c.error_snippet},
      });
    }

    pj["prompt_tests"] = nlohmann::json::array();
    for (const auto &t : p.prompt_tests) {
      pj["prompt_tests"].push_back({
          {"name", t.name},
          {"status", t.status},
          {"latency_ms", t.latency_ms},
          {"rate_limit_headers", t.rate_limit_headers},
          {"answer", t.answer},
          {"error_snippet", t.error_snippet},
      });
    }

    pj["request_traces"] = nlohmann::json::array();
    for (const auto &tr : p.request_traces) {
      pj["request_traces"].push_back({
          {"step", tr.step},
          {"method", tr.method},
          {"url", tr.url},
          {"status", tr.status},
          {"latency_ms", tr.latency_ms},
          {"rate_limit_headers", tr.rate_limit_headers},
          {"response_snippet", tr.response_snippet},
          {"error", tr.error},
      });
    }

    pj["raw_payload"] = p.raw_payload;
    out["providers"].push_back(std::move(pj));
  }

  return out;
}

std::string BuildSummaryText(const AuditReport &report) {
  std::ostringstream oss;
  oss << "API-Tester Audit Summary\n";
  oss << "Generated at (UTC): " << report.generated_at_utc << "\n\n";

  for (const auto &p : report.providers) {
    oss << "Provider: " << p.provider_name << " (" << p.provider_id << ")\n";
    oss << "Key supplied: " << (p.key_supplied ? "yes" : "no") << "\n";
    oss << "Models status: " << p.models_status
        << " | Auth status: " << p.auth_status << "\n";
    oss << "Model used: " << p.model_used << "\n";
    oss << "Working models: " << p.working_models.size()
        << " | Failing models: " << p.failing_models.size() << "\n";
    oss << "Max context seen: " << p.max_context_seen << "\n";
    oss << "Score (reasoning/coding/ax/total): " << p.score_reasoning << "/"
        << p.score_coding << "/" << p.score_axui << "/" << p.score_total
        << "\n";
    oss << "Requests total/success/fail: " << p.total_requests << "/"
        << p.successful_requests << "/" << p.failed_requests
        << " | avg latency(ms): " << p.avg_latency_ms << "\n";
    if (!p.notes.empty())
      oss << "Notes: " << p.notes << "\n";
    if (!p.error_snippet.empty())
      oss << "Error snippet: " << p.error_snippet << "\n";

    if (!p.models_rate_limit_headers.empty()) {
      oss << "Model rate-limit headers:\n";
      for (const auto &[k, v] : p.models_rate_limit_headers)
        oss << "  " << k << ": " << v << "\n";
    }

    if (!p.prompt_tests.empty()) {
      oss << "Prompt tests:\n";
      for (const auto &t : p.prompt_tests) {
        oss << "  - " << t.name << ": status=" << t.status
            << ", latency_ms=" << t.latency_ms << "\n";
      }
    }

    oss << "\n";
  }

  return oss.str();
}

} // namespace llaudit
