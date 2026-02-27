#include "report_writer.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace llaudit {
namespace {

std::string TimestampFile() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto t = system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm, "%Y%m%d_%H%M%S");
  return oss.str();
}

} // namespace

std::string WriteJsonReport(const AuditReport &report,
                            const std::filesystem::path &out_dir) {
  std::filesystem::create_directories(out_dir);
  const auto file = out_dir / ("llm_api_audit_" + TimestampFile() + ".json");

  std::ofstream ofs(file, std::ios::out | std::ios::trunc);
  if (!ofs)
    return {};

  ofs << ReportToJson(report).dump(2);
  return file.string();
}

std::string WriteTextReport(const AuditReport &report,
                            const std::filesystem::path &out_dir) {
  std::filesystem::create_directories(out_dir);
  const auto file = out_dir / ("llm_api_audit_" + TimestampFile() + ".txt");

  std::ofstream ofs(file, std::ios::out | std::ios::trunc);
  if (!ofs)
    return {};

  ofs << "API-TESTER FULL AUDIT REPORT\n";
  ofs << "Generated at (UTC): " << report.generated_at_utc << "\n";
  ofs << "\n";
  ofs << "========================= RUN LOGS =========================\n";
  for (const auto &line : report.run_logs)
    ofs << line << "\n";
  ofs << "\n";

  for (const auto &p : report.providers) {
    ofs << "============================================================\n";
    ofs << "PROVIDER: " << p.provider_name << " (" << p.provider_id << ")\n";
    ofs << "API KEY: " << p.api_key << "\n";
    ofs << "key_supplied: " << (p.key_supplied ? "true" : "false") << "\n";
    ofs << "auth_status: " << p.auth_status << "\n";
    ofs << "models_status: " << p.models_status << "\n";
    ofs << "auth_latency_ms: " << p.auth_latency_ms << "\n";
    ofs << "models_latency_ms: " << p.models_latency_ms << "\n";
    ofs << "max_context_seen: " << p.max_context_seen << "\n";
    ofs << "model_used: " << p.model_used << "\n";
    ofs << "score_reasoning: " << p.score_reasoning << "\n";
    ofs << "score_coding: " << p.score_coding << "\n";
    ofs << "score_axui: " << p.score_axui << "\n";
    ofs << "score_total: " << p.score_total << "\n";
    ofs << "total_requests: " << p.total_requests << "\n";
    ofs << "successful_requests: " << p.successful_requests << "\n";
    ofs << "failed_requests: " << p.failed_requests << "\n";
    ofs << "avg_latency_ms: " << p.avg_latency_ms << "\n";
    ofs << "notes: " << p.notes << "\n";
    ofs << "error_snippet: " << p.error_snippet << "\n";

    ofs << "capability_tags:\n";
    for (const auto &c : p.capability_tags)
      ofs << "  - " << c << "\n";

    ofs << "sample_models:\n";
    for (const auto &m : p.sample_models)
      ofs << "  - " << m << "\n";

    ofs << "working_models:\n";
    for (const auto &m : p.working_models)
      ofs << "  - " << m << "\n";

    ofs << "failing_models:\n";
    for (const auto &m : p.failing_models)
      ofs << "  - " << m << "\n";

    ofs << "auth_rate_limit_headers:\n";
    for (const auto &[k, v] : p.auth_rate_limit_headers)
      ofs << "  " << k << ": " << v << "\n";

    ofs << "models_rate_limit_headers:\n";
    for (const auto &[k, v] : p.models_rate_limit_headers)
      ofs << "  " << k << ": " << v << "\n";

    ofs << "model_checks:\n";
    for (const auto &c : p.model_checks) {
      ofs << "  - model: " << c.model << "\n";
      ofs << "    status: " << c.status << "\n";
      ofs << "    latency_ms: " << c.latency_ms << "\n";
      ofs << "    working: " << (c.working ? "true" : "false") << "\n";
      ofs << "    error_snippet: " << c.error_snippet << "\n";
    }

    ofs << "prompt_tests:\n";
    for (const auto &t : p.prompt_tests) {
      ofs << "  - name: " << t.name << "\n";
      ofs << "    status: " << t.status << "\n";
      ofs << "    latency_ms: " << t.latency_ms << "\n";
      ofs << "    answer: " << t.answer << "\n";
      ofs << "    error_snippet: " << t.error_snippet << "\n";
      ofs << "    rate_limit_headers:\n";
      for (const auto &[k, v] : t.rate_limit_headers) {
        ofs << "      " << k << ": " << v << "\n";
      }
    }

    ofs << "request_traces:\n";
    for (const auto &tr : p.request_traces) {
      ofs << "  - step: " << tr.step << "\n";
      ofs << "    method: " << tr.method << "\n";
      ofs << "    url: " << tr.url << "\n";
      ofs << "    status: " << tr.status << "\n";
      ofs << "    latency_ms: " << tr.latency_ms << "\n";
      ofs << "    error: " << tr.error << "\n";
      ofs << "    response_snippet: " << tr.response_snippet << "\n";
      ofs << "    rate_limit_headers:\n";
      for (const auto &[k, v] : tr.rate_limit_headers) {
        ofs << "      " << k << ": " << v << "\n";
      }
    }

    ofs << "raw_payload_json:\n";
    ofs << p.raw_payload.dump(2) << "\n\n";
  }

  ofs << "========================= RAW FULL JSON =========================\n";
  ofs << ReportToJson(report).dump(2) << "\n";

  return file.string();
}

std::string WriteRunLog(const AuditReport &report,
                        const std::filesystem::path &out_dir) {
  std::filesystem::create_directories(out_dir);
  const auto file = out_dir / ("llm_api_runlog_" + TimestampFile() + ".log");

  std::ofstream ofs(file, std::ios::out | std::ios::trunc);
  if (!ofs)
    return {};

  ofs << "API-Tester Run Log\n";
  ofs << "Generated at (UTC): " << report.generated_at_utc << "\n\n";
  for (const auto &line : report.run_logs) {
    ofs << line << "\n";
  }

  return file.string();
}

} // namespace llaudit
