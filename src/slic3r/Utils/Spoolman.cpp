#include "Spoolman.hpp"
#include "Http.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "nlohmann/json.hpp"
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>

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

bool fetch_spool(const std::string& base_url, const std::string& api_key, int spool_id, SpoolmanSpool& spool)
{
    std::string body;
    std::string error;
    auto        http = Http::post(base_url + "/server/spoolman/proxy");
    if (!api_key.empty())
        http.header("X-Api-Key", api_key);
    http.header("Content-Type", "application/json")
        .set_post_body(nlohmann::json{{"request_method", "GET"}, {"path", "/v1/spool/" + std::to_string(spool_id)}}.dump())
        .timeout_connect(3)
        .timeout_max(5)
        .on_complete([&](std::string response, unsigned) { body = std::move(response); })
        .on_error([&](std::string, std::string err, unsigned status) { error = err + " (HTTP " + std::to_string(status) + ")"; })
        .perform_sync();
    if (body.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "Spoolman: spool " << spool_id << " not read: " << error;
        return false;
    }

    auto json = nlohmann::json::parse(body, nullptr, false);
    // Moonraker wraps the Spoolman response in "result".
    if (json.is_object() && json.contains("result"))
        json = json["result"];
    if (!json.is_object() || !json.contains("filament") || !json["filament"].is_object()) {
        BOOST_LOG_TRIVIAL(warning) << "Spoolman: unexpected response for spool " << spool_id;
        return false;
    }

    const nlohmann::json& filament = json["filament"];
    spool                  = SpoolmanSpool();
    spool.id               = spool_id;
    spool.filament_id      = int(json_number(filament, "id"));
    spool.name             = json_string(filament, "name");
    spool.material         = json_string(filament, "material");
    spool.density          = json_number(filament, "density");
    spool.diameter         = json_number(filament, "diameter");
    spool.price            = json_number(json, "price");
    spool.initial_weight   = json_number(json, "initial_weight");
    spool.remaining_weight = json_number(json, "remaining_weight");
    if (spool.price == 0.)
        spool.price = json_number(filament, "price");
    if (spool.initial_weight == 0.)
        spool.initial_weight = json_number(filament, "weight");
    if (filament.contains("vendor") && filament["vendor"].is_object())
        spool.vendor = json_string(filament["vendor"], "name");
    return spool.filament_id > 0;
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
