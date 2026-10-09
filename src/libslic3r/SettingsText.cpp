#include "libslic3r/SettingsText.hpp"

#include <algorithm>
#include <exception>
#include <sstream>

#include <boost/algorithm/string/trim.hpp>

#include "libslic3r/Config.hpp"

namespace Slic3r {

bool is_settings_text_identity_key(const std::string& key)
{
    static const std::vector<std::string> identity_keys{
        "inherits", "print_settings_id", "filament_settings_id", "printer_settings_id", "compatible_printers",
        "compatible_printers_condition", "compatible_prints", "compatible_prints_condition", "renamed_from",
        "version", "from", "name", "setting_id", "filament_id", "printer_model", "printer_variant",
        "default_print_profile", "default_filament_profile", "print_compatible_printers"};
    return std::find(identity_keys.begin(), identity_keys.end(), key) != identity_keys.end();
}

std::string settings_to_text(const DynamicPrintConfig& config, const std::vector<std::string>& keys, const std::vector<std::string>& header)
{
    std::ostringstream out;
    for (const std::string& line : header)
        out << "# " << line << "\n";
    for (const std::string& key : keys)
        if (!is_settings_text_identity_key(key) && config.has(key))
            out << key << " = " << config.opt_serialize(key) << "\n";
    return out.str();
}

SettingsTextParseResult settings_from_text(const std::string& text)
{
    SettingsTextParseResult result;
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Enable);
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        boost::algorithm::trim(line);
        if (!line.empty() && line.front() == ';') {
            line.erase(0, 1);
            boost::algorithm::trim(line);
        }
        if (line.empty() || line.front() == '#')
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key   = boost::algorithm::trim_copy(line.substr(0, eq));
        std::string value = boost::algorithm::trim_copy(line.substr(eq + 1));
        if (key.empty())
            continue;
        if (is_settings_text_identity_key(key)) {
            result.skipped_keys.push_back(key);
            continue;
        }
        // set_deserialize_nothrow() renames legacy keys, but still throws for a key it has no definition for.
        try {
            if (!result.config.set_deserialize_nothrow(key, value, substitutions))
                result.invalid_keys.push_back(key);
        } catch (const UnknownOptionException&) {
            result.skipped_keys.push_back(key);
        } catch (const std::exception&) {
            result.invalid_keys.push_back(key);
        }
    }
    // Keys that handle_legacy() dropped as obsolete.
    for (const std::string& key : substitutions.unrecogized_keys)
        result.skipped_keys.push_back(key);
    return result;
}

} // namespace Slic3r
