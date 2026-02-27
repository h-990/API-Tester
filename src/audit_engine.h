#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace llaudit {

struct PromptTest {
  std::string name;
  long status = -1;
  long latency_ms = -1;
  std::map<std::string, std::string> rate_limit_headers;
  std::string answer;
  std::string error_snippet;
};

struct ModelCheck {
  std::string model;
  long status = -1;
  long latency_ms = -1;
  bool working = false;
  std::string error_snippet;
};

struct RequestTrace {
  std::string step;
  std::string method;
  std::string url;
  long status = -1;
  long latency_ms = -1;
  std::map<std::string, std::string> rate_limit_headers;
  std::string response_snippet;
  std::string error;
};

struct ProviderAudit {
  std::string provider_id;
  std::string provider_name;
  std::string api_key;
  bool key_supplied = false;

  long auth_status = -1;
  long models_status = -1;
  long auth_latency_ms = -1;
  long models_latency_ms = -1;

  std::map<std::string, std::string> auth_rate_limit_headers;
  std::map<std::string, std::string> models_rate_limit_headers;

  std::vector<std::string> sample_models;
  std::vector<std::string> capability_tags;
  std::vector<std::string> working_models;
  std::vector<std::string> failing_models;

  std::string model_used;
  long long max_context_seen = -1;

  std::vector<ModelCheck> model_checks;
  std::vector<PromptTest> prompt_tests;
  std::vector<RequestTrace> request_traces;

  int score_reasoning = 0;
  int score_coding = 0;
  int score_axui = 0;
  int score_total = 0;

  int total_requests = 0;
  int successful_requests = 0;
  int failed_requests = 0;
  long avg_latency_ms = -1;

  std::string notes;
  std::string error_snippet;
  nlohmann::json raw_payload;
};

struct AuditReport {
  std::string generated_at_utc;
  std::vector<std::string> run_logs;
  std::vector<ProviderAudit> providers;
};

using LogFn = std::function<void(const std::string&)>;

class AuditEngine {
 public:
  AuditReport Run(const std::map<std::string, std::string>& keys, const LogFn& log,
                  const std::atomic<bool>& cancel_requested);
};

nlohmann::json ReportToJson(const AuditReport& report);
std::string BuildSummaryText(const AuditReport& report);

}  // namespace llaudit
