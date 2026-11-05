#pragma once

#include "KindleDocument.h"
#include <string>
#include <string_view>

// Plus-only import adapter. Original paths remain the reader/progress identity;
// the generated ZIP is private, disposable reading cache, never a user export.
namespace kindle {
inline constexpr char kMarkerName[] = "/kindle.v2";
bool supported(std::string_view path);
bool metadata(const std::string& path, std::string& title, std::string& author, Error& error);
bool prepare(const std::string& path, const std::string& cache, std::string& archive,
             bool& rebuilt, Error& error, CancelCheck cancellation = {});
}  // namespace kindle
