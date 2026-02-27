#pragma once

#include <filesystem>
#include <string>

#include "audit_engine.h"

namespace llaudit {

std::string WriteJsonReport(const AuditReport& report, const std::filesystem::path& out_dir);
std::string WriteTextReport(const AuditReport& report, const std::filesystem::path& out_dir);
std::string WriteRunLog(const AuditReport& report, const std::filesystem::path& out_dir);

}  // namespace llaudit
