#include "PlateSummaryDialog.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/font.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Slicing.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"
#include "format.hpp"
#include "Plater.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/RadioGroup.hpp"

namespace Slic3r { namespace GUI {

namespace {

// Plate config keys that are bookkeeping rather than settings the user changed.
const std::set<std::string> hidden_plate_keys = {"filament_map", "filament_map_mode", "filament_nozzle_map", "filament_volume_map",
                                                 "other_layers_print_sequence_nums", "imex_head_filament_map"};

wxString option_label(const std::string& key)
{
    const ConfigOptionDef* def = print_config_def.get(key);
    if (def == nullptr)
        return from_u8(key);
    const std::string& label = def->full_label.empty() ? def->label : def->full_label;
    return label.empty() ? from_u8(key) : _(label);
}

wxString option_value(const std::string& key, const ConfigOption& opt)
{
    const ConfigOptionDef* def = print_config_def.get(key);
    if (def != nullptr && def->type == coBool)
        return opt.getBool() ? _L("Yes") : _L("No");
    const std::string value = opt.serialize();
    if (def != nullptr && def->type == coEnum && def->enum_labels.size() == def->enum_values.size()) {
        auto it = std::find(def->enum_values.begin(), def->enum_values.end(), value);
        if (it != def->enum_values.end())
            return _(def->enum_labels[std::distance(def->enum_values.begin(), it)]);
    }
    return from_u8(value);
}

// One "Label: value" line per key the config has, indented.
void append_settings(wxString& out, const DynamicPrintConfig& config, const std::vector<std::string>& keys, const wxString& indent)
{
    for (const std::string& key : keys)
        if (const ConfigOption* opt = config.option(key))
            out += indent + option_label(key) + ": " + option_value(key, *opt) + "\n";
}

wxString filament_text(int filament_id)
{
    const PresetBundle& bundle = *wxGetApp().preset_bundle;
    const auto*         colors = bundle.project_config.option<ConfigOptionStrings>("filament_colour");
    const size_t        idx    = size_t(filament_id - 1);
    wxString            text   = wxString::Format("%d", filament_id);
    if (idx < bundle.filament_presets.size())
        text += ": " + from_u8(bundle.filament_presets[idx]);
    if (colors != nullptr && idx < colors->values.size())
        text += " (" + from_u8(colors->values[idx]) + ")";
    return text;
}

wxString volume_type_name(const ModelVolume& volume)
{
    switch (volume.type()) {
    case ModelVolumeType::MODEL_PART: return _L("Part");
    case ModelVolumeType::NEGATIVE_VOLUME: return _L("Negative Part");
    case ModelVolumeType::PARAMETER_MODIFIER: return _L("Modifier");
    case ModelVolumeType::SUPPORT_BLOCKER: return _L("Support Blocker");
    case ModelVolumeType::SUPPORT_ENFORCER: return _L("Support Enforcer");
    case ModelVolumeType::PAINTED_MODIFIER: return _L("Painted modifier");
    default: return volume.is_precise_seam() ? _L("Seam modifier") : _L("Part");
    }
}

std::vector<wxString> painting(const ModelVolume& volume)
{
    std::vector<wxString> painted;
    if (!volume.mmu_segmentation_facets.empty())
        painted.push_back(_L("colors"));
    if (!volume.supported_facets.empty())
        painted.push_back(_L("supports"));
    if (!volume.seam_facets.empty())
        painted.push_back(_L("seams"));
    if (!volume.fuzzy_skin_facets.empty())
        painted.push_back(_L("fuzzy skin"));
    return painted;
}

wxString join(const std::vector<wxString>& items)
{
    wxString out;
    for (const wxString& item : items)
        out += (out.empty() ? "" : ", ") + item;
    return out;
}

// The object's own settings except its filament, which the header line shows.
std::vector<std::string> override_keys(const ModelConfig& config)
{
    std::vector<std::string> keys = config.keys();
    keys.erase(std::remove(keys.begin(), keys.end(), "extruder"), keys.end());
    return keys;
}

void append_object(wxString& out, const ModelObject& object, int instances_on_plate)
{
    const ConfigOption* extruder = object.config.option("extruder");
    const int           filament = (extruder == nullptr || extruder->getInt() == 0) ? 1 : extruder->getInt();

    out += from_u8(object.name) + "\n";
    // TRN Plate summary line under an object. %1% is a filament number, name and color.
    out += "  " + format_wxstr(_L("Filament: %1%"), filament_text(filament)) + "\n";
    if (instances_on_plate > 1)
        // TRN Plate summary line under an object: how many copies of it are on the plate.
        out += "  " + format_wxstr(_L("Copies on this plate: %1%"), instances_on_plate) + "\n";

    const std::vector<std::string> object_keys = override_keys(object.config);
    if (!object_keys.empty()) {
        out += "  " + _L("Object settings") + ":\n";
        append_settings(out, object.config.get(), object_keys, "    ");
    }

    // A single part is the object itself; list parts only when there are several or other kinds of volumes.
    const bool list_parts = object.volumes.size() > 1;
    std::vector<wxString> object_painting;
    for (const ModelVolume* volume : object.volumes) {
        for (const wxString& kind : painting(*volume))
            if (std::find(object_painting.begin(), object_painting.end(), kind) == object_painting.end())
                object_painting.push_back(kind);
        if (!list_parts)
            continue;

        wxString line = "  " + volume_type_name(*volume) + " \"" + from_u8(volume->name) + "\"";
        if (volume->is_model_part() || volume->is_region_modifier()) {
            const ConfigOption* vol_extruder = volume->config.option("extruder");
            if (vol_extruder != nullptr && vol_extruder->getInt() != 0)
                line += ", " + format_wxstr(_L("Filament: %1%"), filament_text(vol_extruder->getInt()));
        }
        out += line + "\n";
        append_settings(out, volume->config.get(), override_keys(volume->config), "    ");
    }

    for (const auto& [range, config] : object.layer_config_ranges) {
        // TRN Plate summary line: a height range modifier. %1% and %2% are heights in mm.
        out += "  " + format_wxstr(_L("Height range %1%-%2% mm"), wxString::Format("%.2f", range.first),
                                   wxString::Format("%.2f", range.second)) + "\n";
        const ConfigOption* range_extruder = config.option("extruder");
        if (range_extruder != nullptr && range_extruder->getInt() != 0)
            out += "    " + format_wxstr(_L("Filament: %1%"), filament_text(range_extruder->getInt())) + "\n";
        append_settings(out, config.get(), override_keys(config), "    ");
    }

    if (!object_painting.empty())
        // TRN Plate summary line: which kinds of painting an object has, such as "colors, supports".
        out += "  " + format_wxstr(_L("Painted: %1%"), join(object_painting)) + "\n";
}

// The plate's summary, or with obj_idx >= 0 the summary of that object on the plate.
wxString build_summary(int plate_idx, int obj_idx)
{
    Plater*        plater = wxGetApp().plater();
    PresetBundle&  bundle = *wxGetApp().preset_bundle;
    PartPlate*     plate  = plater->get_partplate_list().get_plate(plate_idx);
    const Model&   model  = plater->model();
    if (plate == nullptr)
        return {};

    wxString out;
    // TRN Plate summary title. %1% is the plate number.
    out += format_wxstr(_L("Plate %1%"), plate_idx + 1);
    if (!plate->get_plate_name().empty())
        out += ": " + from_u8(plate->get_plate_name());
    out += "\n";
    if (obj_idx >= 0 && obj_idx < int(model.objects.size()))
        // TRN Plate summary title for one object. %1% is the object name.
        out += format_wxstr(_L("Object: %1%"), from_u8(model.objects[obj_idx]->name)) + "\n";

    const PresetCollection& printers = bundle.printers;
    const PresetCollection& prints   = bundle.prints;
    out += "\n" + format_wxstr(_L("Printer: %1%"), from_u8(printers.get_edited_preset().name)) + "\n";
    out += format_wxstr(_L("Process: %1%"), from_u8(prints.get_edited_preset().name)) + "\n";

    // Filaments the plate, or the object, uses.
    std::vector<int> filaments;
    if (obj_idx >= 0 && obj_idx < int(model.objects.size())) {
        std::set<int> used;
        for (const ModelVolume* volume : model.objects[obj_idx]->volumes)
            for (int id : volume->get_extruders())
                used.insert(id);
        filaments.assign(used.begin(), used.end());
    } else
        filaments = plate->get_extruders();
    if (!filaments.empty()) {
        out += "\n" + _L("Filaments") + ":\n";
        for (int id : filaments)
            out += "  " + filament_text(id) + "\n";
    }

    // The settings the process tab marks with a revert arrow: edits not saved to the process preset.
    const std::vector<std::string> process_changes = prints.current_dirty_options(true);
    if (!process_changes.empty()) {
        out += "\n" + _L("Unsaved process changes") + ":\n";
        const DynamicPrintConfig& saved = prints.get_selected_preset().config;
        for (const std::string& key : process_changes) {
            const ConfigOption* opt = prints.get_edited_preset().config.option(key);
            if (opt == nullptr)
                continue;
            wxString line = "  " + option_label(key) + ": " + option_value(key, *opt);
            if (const ConfigOption* saved_opt = saved.option(key))
                // TRN Plate summary: the value a setting has in the saved preset. %1% is a value.
                line += " " + format_wxstr(_L("(saved: %1%)"), option_value(key, *saved_opt));
            out += line + "\n";
        }
    }

    // A plate stores only the settings set on it; the ones holding their "same as global" value are skipped.
    const DynamicPrintConfig& plate_config = *plate->config();
    std::vector<std::string>  plate_keys;
    for (const std::string& key : plate_config.keys()) {
        if (hidden_plate_keys.count(key) != 0)
            continue;
        if (key == "curr_bed_type" && plate->get_bed_type() == BedType::btDefault)
            continue;
        if (key == "print_sequence" && plate->get_print_seq() == PrintSequence::ByDefault)
            continue;
        const ConfigOption* opt = plate_config.option(key);
        if (opt->is_vector() && static_cast<const ConfigOptionVectorBase*>(opt)->empty())
            continue;
        plate_keys.push_back(key);
    }
    if (!plate_keys.empty()) {
        out += "\n" + _L("Plate settings") + ":\n";
        append_settings(out, plate_config, plate_keys, "  ");
    }

    if (!plate->get_notes().empty()) {
        out += "\n" + _L("Notes") + ":\n";
        wxString notes = from_u8(plate->get_notes());
        notes.Replace("\n", "\n  ");
        out += "  " + notes + "\n";
    }

    std::vector<int> object_indices;
    if (obj_idx >= 0)
        object_indices.push_back(obj_idx);
    else
        for (int i = 0; i < int(model.objects.size()); ++i)
            if (plate->contain_any_instance_totally(i))
                object_indices.push_back(i);
    if (!object_indices.empty()) {
        out += "\n" + _L("Objects") + ":\n";
        for (int i : object_indices) {
            if (i < 0 || i >= int(model.objects.size()))
                continue;
            int instances = 0;
            for (int inst = 0; inst < int(model.objects[i]->instances.size()); ++inst)
                if (plate->contain_instance_totally(i, inst))
                    ++instances;
            out += "\n";
            append_object(out, *model.objects[i], instances);
        }
    }
    return out;
}

} // namespace

PlateSummaryDialog::PlateSummaryDialog(wxWindow* parent, bool object_view, int plate_idx, int obj_idx)
    : DPIDialog(parent, wxID_ANY, _L("Summary"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
    , m_plate_idx(plate_idx)
    , m_obj_idx(obj_idx)
    , m_obj_plate_idx(plate_idx)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    if (m_obj_idx >= 0) {
        // The plate of the selected object's first instance that sits on a plate.
        const ModelObject* object = wxGetApp().plater()->model().objects[m_obj_idx];
        for (int inst = 0; inst < int(object->instances.size()); ++inst) {
            const int idx = wxGetApp().plater()->get_partplate_list().find_instance_belongs(m_obj_idx, inst);
            if (idx >= 0) {
                m_obj_plate_idx = idx;
                break;
            }
        }
    }

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    m_view_choice = new RadioGroup(this, {_L("Plate"), _L("Object")}, wxHORIZONTAL);
    m_view_choice->SetSelection(object_view && m_obj_idx >= 0 ? 1 : 0);
    if (m_obj_idx < 0) {
        m_view_choice->Enable(false);
        m_view_choice->SetToolTip(_L("Select an object to show its summary."));
    }
    m_view_choice->Bind(wxEVT_RADIOBOX, [this](wxCommandEvent& e) {
        // The Plate view of an object's summary shows the plate that object is on.
        if (m_view_choice->GetSelection() == 0)
            m_plate_idx = m_obj_plate_idx;
        update_text();
        e.Skip();
    });
    sizer->Add(m_view_choice, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

    m_text = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(600), FromDIP(420)),
                            wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    m_text->SetFont(wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE)));
    sizer->Add(m_text, 1, wxEXPAND | wxALL, FromDIP(10));

    auto* btns = new DialogButtons(this, {"Copy", "Cancel"});
    btns->GetButtonFromID(wxID_COPY)->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        wxClipboardLocker lock;
        if (!lock)
            return;
        wxTheClipboard->SetData(new wxTextDataObject(m_text->GetValue()));
        EndModal(wxID_OK);
    });
    btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    sizer->Add(btns, 0, wxEXPAND);

    update_text();
    SetSizerAndFit(sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void PlateSummaryDialog::update_text()
{
    const bool object_view = m_view_choice->GetSelection() == 1 && m_obj_idx >= 0;
    m_text->SetValue(object_view ? build_summary(m_obj_plate_idx, m_obj_idx) : build_summary(m_plate_idx, -1));
}

void PlateSummaryDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    m_text->SetMinSize(wxSize(FromDIP(600), FromDIP(420)));
    Fit();
    Refresh();
}

}} // namespace Slic3r::GUI
