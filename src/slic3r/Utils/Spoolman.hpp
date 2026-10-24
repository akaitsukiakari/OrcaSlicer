#pragma once

#include <map>
#include <string>
#include <vector>

namespace Slic3r {

class PresetCollection;

// A Spoolman spool loaded in a printer slot.
struct SpoolmanSpool
{
    int         id          = 0;
    int         filament_id = 0;
    std::string vendor;
    std::string name;
    std::string material;
    double      price            = 0.; // price of the whole spool
    double      initial_weight   = 0.; // grams of filament on a full spool
    double      remaining_weight = 0.; // grams
    double      density          = 0.;
    double      diameter         = 0.;
    std::string location;
    // "#RRGGBB" colors of the filament, more than one for a multicolor filament.
    std::vector<std::string> colors;
    // The colors change along the filament rather than lying side by side.
    bool gradient = false;

    // "GEEETECH PLA Black"
    std::string label() const;
    // Price per kg of filament, 0 when Spoolman has no price or spool weight.
    double cost_per_kg() const;
};

// Spoolman data of the spools in the slots of a Moonraker printer. Moonraker proxies the
// Spoolman API, so the printer's own connection is used and Orca needs no Spoolman address.
// Listing every spool uses the Spoolman address in the app config when set, otherwise that proxy.
namespace Spoolman {

// App config key of the Spoolman address, "http://host:7912".
extern const char* const URL_KEY;
// App config key of the option to choose a filament's color by choosing a Spoolman spool.
extern const char* const PICKER_KEY;

// Linked preset value of a Spoolman filament the user chose not to link to a preset.
extern const std::string NOT_LINKED;

// Reads a spool through the Moonraker Spoolman proxy. Runs synchronously.
bool fetch_spool(const std::string& base_url, const std::string& api_key, int spool_id, SpoolmanSpool& spool);

// Reads the spools that aren't archived. Runs synchronously. Fails with an error message when
// there is no Spoolman address and no printer has been read through its proxy since Orca started.
bool fetch_spools(std::vector<SpoolmanSpool>& spools, std::string& error);

// Spools found in the printer slots by the last filament sync, by slot index (ams_id * 4 + slot_id).
std::map<int, SpoolmanSpool>& slot_spools();

// A plater filament synced from a slot holding a spool, with the preset the sync selected for it.
struct SyncedFilament
{
    SpoolmanSpool spool;
    std::string   preset;
};
// The synced filaments by filament index. One whose preset was changed since no longer holds that spool.
std::map<int, SyncedFilament>& synced_filaments();

// The linked presets of the spools in slot_spools(), by "ams_id:slot_id" as PresetBundle::sync_ams_list() reads them.
std::map<std::string, std::string> slot_presets();

// The preset the user linked to a Spoolman filament: empty when not asked yet, NOT_LINKED when
// they chose not to link it.
std::string linked_preset(int filament_id);
void        set_linked_preset(int filament_id, const std::string& label, const std::string& preset);
// The links the user made, by Spoolman filament id, with the filament label and the preset name.
std::map<int, std::pair<std::string, std::string>> links();

// Writes the spool's cost per kg and density into a user filament preset and saves it, when they
// differ from the preset's. Returns true when the preset was saved.
bool update_preset(PresetCollection& filaments, const std::string& preset_name, const SpoolmanSpool& spool);

} // namespace Spoolman
} // namespace Slic3r
