#include "Spoolman.hpp"
#include "Http.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

namespace {

constexpr const char* SECTION_LINKS  = "spoolman_links";
constexpr const char* SECTION_LABELS = "spoolman_labels";

double json_number(const nlohmann::json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : 0.;
}

std::string json_string(const nlohmann::json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// "#RRGGBB" of a Spoolman "RRGGBB" or "RRGGBBAA" color, empty when it isn't one.
std::string hex_color(const std::string& hex)
{
    if (hex.size() < 6 || !std::all_of(hex.begin(), hex.begin() + 6, [](unsigned char c) { return std::isxdigit(c); }))
        return {};
    return "#" + boost::algorithm::to_upper_copy(hex.substr(0, 6));
}

// The Moonraker printer whose Spoolman proxy last read a spool.
struct Proxy
{
    std::string base_url;
    std::string api_key;
};
Proxy& last_proxy()
{
    static Proxy proxy;
    return proxy;
}

// Spoolman API GET of path ("/v1/spool"), from Spoolman itself or through the Moonraker proxy
// of a printer. Returns a discarded value and sets error when nothing was read.
nlohmann::json get_json(const std::string& base_url, const std::string& api_key, const std::string& path, bool proxy, std::string& error)
{
    std::string body;
    auto        http = proxy ? Http::post(base_url + "/server/spoolman/proxy") : Http::get(base_url + "/api" + path);
    if (proxy) {
        if (!api_key.empty())
            http.header("X-Api-Key", api_key);
        http.header("Content-Type", "application/json")
            .set_post_body(nlohmann::json{{"request_method", "GET"}, {"path", path}}.dump());
    }
    http.timeout_connect(3)
        .timeout_max(5)
        .on_complete([&](std::string response, unsigned) { body = std::move(response); })
        .on_error([&](std::string, std::string err, unsigned status) { error = err + " (HTTP " + std::to_string(status) + ")"; })
        .perform_sync();
    if (body.empty()) {
        if (error.empty())
            error = "empty response";
        return nlohmann::json(nlohmann::json::value_t::discarded);
    }

    auto json = nlohmann::json::parse(body, nullptr, false);
    // Moonraker wraps the Spoolman response in "result".
    if (proxy && json.is_object() && json.contains("result"))
        json = json["result"];
    return json;
}

bool parse_spool(const nlohmann::json& json, SpoolmanSpool& spool)
{
    if (!json.is_object() || !json.contains("filament") || !json["filament"].is_object())
        return false;

    const nlohmann::json& filament = json["filament"];
    spool                  = SpoolmanSpool();
    spool.id               = int(json_number(json, "id"));
    spool.filament_id      = int(json_number(filament, "id"));
    spool.name             = json_string(filament, "name");
    spool.material         = json_string(filament, "material");
    spool.density          = json_number(filament, "density");
    spool.diameter         = json_number(filament, "diameter");
    spool.price            = json_number(json, "price");
    spool.initial_weight   = json_number(json, "initial_weight");
    spool.remaining_weight = json_number(json, "remaining_weight");
    spool.location         = json_string(json, "location");
    if (spool.price == 0.)
        spool.price = json_number(filament, "price");
    if (spool.initial_weight == 0.)
        spool.initial_weight = json_number(filament, "weight");
    if (filament.contains("vendor") && filament["vendor"].is_object())
        spool.vendor = json_string(filament["vendor"], "name");

    const std::string        multi_color = json_string(filament, "multi_color_hexes");
    std::vector<std::string> hexes;
    boost::algorithm::split(hexes, multi_color, boost::algorithm::is_any_of(","));
    for (const std::string& hex : hexes)
        if (std::string color = hex_color(boost::algorithm::trim_copy(hex)); !color.empty())
            spool.colors.push_back(std::move(color));
    if (spool.colors.empty())
        if (std::string color = hex_color(json_string(filament, "color_hex")); !color.empty())
            spool.colors.push_back(std::move(color));
    spool.gradient = spool.colors.size() > 1 && json_string(filament, "multi_color_direction") == "longitudinal";
    return spool.id > 0 && spool.filament_id > 0;
}

} // anonymous namespace

std::string SpoolmanSpool::label() const
{
    std::string out = vendor;
    for (const std::string* part : {&material, &name})
        if (!part->empty())
            out += (out.empty() ? "" : " ") + *part;
    return out.empty() ? "Spool #" + std::to_string(id) : out;
}

double SpoolmanSpool::cost_per_kg() const { return initial_weight > 0. ? price * 1000. / initial_weight : 0.; }

namespace Spoolman {

const std::string NOT_LINKED = "-";
const char* const URL_KEY    = "spoolman_url";
const char* const PICKER_KEY = "spoolman_color_picker";

bool fetch_spool(const std::string& base_url, const std::string& api_key, int spool_id, SpoolmanSpool& spool)
{
    last_proxy() = {base_url, api_key};
    std::string    error;
    nlohmann::json json = get_json(base_url, api_key, "/v1/spool/" + std::to_string(spool_id), true, error);
    if (json.is_discarded() && !error.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "Spoolman: spool " << spool_id << " not read: " << error;
        return false;
    }
    if (!parse_spool(json, spool)) {
        BOOST_LOG_TRIVIAL(warning) << "Spoolman: unexpected response for spool " << spool_id;
        return false;
    }
    return true;
}

bool fetch_spools(std::vector<SpoolmanSpool>& spools, std::string& error)
{
    spools.clear();
    error.clear();
    std::string url = GUI::wxGetApp().app_config->get(URL_KEY);
    boost::algorithm::trim(url);
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    if (!url.empty() && url.find("://") == std::string::npos)
        url = "http://" + url;
    const bool proxy = url.empty();
    if (proxy && last_proxy().base_url.empty())
        return false;

    nlohmann::json json = proxy ? get_json(last_proxy().base_url, last_proxy().api_key, "/v1/spool", true, error) :
                                  get_json(url, "", "/v1/spool", false, error);
    if (!json.is_array()) {
        if (error.empty())
            error = "unexpected response";
        BOOST_LOG_TRIVIAL(warning) << "Spoolman: spools not read: " << error;
        return false;
    }
    for (const nlohmann::json& item : json) {
        SpoolmanSpool spool;
        if (parse_spool(item, spool))
            spools.push_back(std::move(spool));
    }
    std::stable_sort(spools.begin(), spools.end(), [](const SpoolmanSpool& l, const SpoolmanSpool& r) { return l.label() < r.label(); });
    return true;
}

std::map<int, SpoolmanSpool>& slot_spools()
{
    static std::map<int, SpoolmanSpool> spools;
    return spools;
}

std::map<int, SyncedFilament>& synced_filaments()
{
    static std::map<int, SyncedFilament> filaments;
    return filaments;
}

std::map<std::string, std::string> slot_presets()
{
    std::map<std::string, std::string> out;
    for (const auto& [slot, spool] : slot_spools()) {
        const std::string preset = linked_preset(spool.filament_id);
        // Four slots per unit, as MoonrakerPrinterAgent::build_ams_payload() lays them out.
        if (!preset.empty() && preset != NOT_LINKED)
            out[std::to_string(slot / 4) + ":" + std::to_string(slot % 4)] = preset;
    }
    return out;
}

std::string linked_preset(int filament_id) { return GUI::wxGetApp().app_config->get(SECTION_LINKS, std::to_string(filament_id)); }

void set_linked_preset(int filament_id, const std::string& label, const std::string& preset)
{
    AppConfig*        config = GUI::wxGetApp().app_config;
    const std::string key    = std::to_string(filament_id);
    if (preset.empty()) {
        config->erase(SECTION_LINKS, key);
        config->erase(SECTION_LABELS, key);
    } else {
        config->set(SECTION_LINKS, key, preset);
        config->set(SECTION_LABELS, key, label);
    }
}

std::map<int, std::pair<std::string, std::string>> links()
{
    std::map<int, std::pair<std::string, std::string>> out;
    AppConfig*                                         config = GUI::wxGetApp().app_config;
    if (!config->has_section(SECTION_LINKS))
        return out;
    for (const auto& [key, preset] : config->get_section(SECTION_LINKS)) {
        const int id = std::atoi(key.c_str());
        if (id > 0)
            out[id] = {config->get(SECTION_LABELS, key), preset};
    }
    return out;
}

bool update_preset(PresetCollection& filaments, const std::string& preset_name, const SpoolmanSpool& spool)
{
    Preset* preset = filaments.find_preset(preset_name);
    if (!preset || !preset->is_user())
        return false;

    std::map<std::string, double> values;
    if (spool.cost_per_kg() > 0.)
        values["filament_cost"] = std::round(spool.cost_per_kg() * 100.) / 100.;
    if (spool.density > 0.)
        values["filament_density"] = spool.density;

    bool       changed = false;
    const bool edited  = filaments.get_selected_preset_name() == preset->name;
    for (const auto& [key, value] : values) {
        auto* opt = preset->config.option<ConfigOptionFloats>(key);
        if (!opt || opt->values.empty() || std::abs(opt->values.front() - value) < 1e-6)
            continue;
        std::fill(opt->values.begin(), opt->values.end(), value);
        // The selected preset is shown with its edited copy, which would otherwise show the change as unsaved.
        if (edited)
            if (auto* edited_opt = filaments.get_edited_preset().config.option<ConfigOptionFloats>(key))
                std::fill(edited_opt->values.begin(), edited_opt->values.end(), value);
        changed = true;
    }
    if (!changed)
        return false;

    // Saved as save_current_preset() does, without selecting the preset.
    const Preset* parent = filaments.get_preset_parent(*preset);
    preset->save(parent ? const_cast<DynamicPrintConfig*>(&parent->config) : nullptr);
    preset->sync_info = "update";
    preset->save_info();
    BOOST_LOG_TRIVIAL(info) << "Spoolman: updated cost and density of " << preset->name << " from spool " << spool.id;
    return true;
}

} // namespace Spoolman
} // namespace Slic3r
