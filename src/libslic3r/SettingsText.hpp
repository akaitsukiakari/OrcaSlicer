#pragma once

#include <string>
#include <vector>

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

// Plain-text settings format used to copy and paste presets: one "key = value" line per setting, values
// serialized the same way as the configuration block at the end of exported G-code. Lines starting with
// '#' are comments; a leading ';' is dropped so that the G-code configuration block can be pasted as is.

// Keys that identify a preset rather than describe how to print. They are never exported or imported.
bool is_settings_text_identity_key(const std::string& key);

// Writes the given keys of config (skipping identity keys and keys config does not have), preceded by
// the header lines, each written as a '#' comment.
std::string settings_to_text(const DynamicPrintConfig& config, const std::vector<std::string>& keys, const std::vector<std::string>& header);

struct SettingsTextParseResult
{
    DynamicPrintConfig       config;
    // Keys this version does not know, or identity keys, which are not imported.
    std::vector<std::string> skipped_keys;
    // Keys whose value could not be parsed.
    std::vector<std::string> invalid_keys;
};

SettingsTextParseResult settings_from_text(const std::string& text);

} // namespace Slic3r
