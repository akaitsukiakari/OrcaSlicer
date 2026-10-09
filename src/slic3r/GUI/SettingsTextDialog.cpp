#include "SettingsTextDialog.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/font.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/SettingsText.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "Tab.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/RadioGroup.hpp"

namespace Slic3r { namespace GUI {

namespace {

struct PresetTypeInfo
{
    Preset::Type type;
    const char*  name; // English: written into the exported text, which is not translated
};

const std::array<PresetTypeInfo, 3> preset_types{{
    {Preset::TYPE_PRINT, "Process"},
    {Preset::TYPE_FILAMENT, "Filament"},
    {Preset::TYPE_PRINTER, "Printer"},
}};

PresetCollection& preset_collection(Preset::Type type)
{
    PresetBundle& bundle = *wxGetApp().preset_bundle;
    switch (type) {
    case Preset::TYPE_FILAMENT: return bundle.filaments;
    case Preset::TYPE_PRINTER: return bundle.printers;
    default: return bundle.prints;
    }
}

const std::vector<std::string>& preset_options(Preset::Type type)
{
    switch (type) {
    case Preset::TYPE_FILAMENT: return Preset::filament_options();
    case Preset::TYPE_PRINTER: return Preset::printer_options();
    default: return Preset::print_options();
    }
}

wxString key_list(const std::vector<std::string>& keys)
{
    const size_t max_shown = 10;
    wxString     out;
    for (size_t i = 0; i < keys.size() && i < max_shown; ++i)
        out += (i == 0 ? "" : ", ") + from_u8(keys[i]);
    if (keys.size() > max_shown)
        out += ", ...";
    return out;
}

} // namespace

SettingsTextDialog::SettingsTextDialog(wxWindow* parent, Mode mode)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY,
                mode == Mode::Export ? _L("Copy Settings as Text") : _L("Paste Settings as Text"), wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
    , m_mode(mode)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    auto* sizer = new wxBoxSizer(wxVERTICAL);

    if (m_mode == Mode::Export) {
        auto* choices = new wxFlexGridSizer(2, FromDIP(6), FromDIP(12));
        choices->Add(new wxStaticText(this, wxID_ANY, _L("Settings") + ":"), 0, wxALIGN_CENTER_VERTICAL);
        m_type_choice = new RadioGroup(this, {_L("Process"), _L("Filament"), _L("Printer")}, wxHORIZONTAL);
        choices->Add(m_type_choice, 0, wxALIGN_CENTER_VERTICAL);
        choices->Add(new wxStaticText(this, wxID_ANY, _L("Include") + ":"), 0, wxALIGN_CENTER_VERTICAL);
        m_scope_choice = new RadioGroup(this, {_L("Changes from the system preset"), _L("All settings")}, wxHORIZONTAL);
        choices->Add(m_scope_choice, 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(choices, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        m_type_choice->Bind(wxEVT_RADIOBOX, [this](wxCommandEvent& e) { update_export_text(); e.Skip(); });
        m_scope_choice->Bind(wxEVT_RADIOBOX, [this](wxCommandEvent& e) { update_export_text(); e.Skip(); });
    } else {
        auto* hint = new wxStaticText(this, wxID_ANY,
                                      _L("Paste \"key = value\" lines, such as text copied from this dialog or the settings block at "
                                         "the end of a G-code file. The values are applied to the current process, filament and "
                                         "printer presets as unsaved changes."));
        hint->Wrap(FromDIP(560));
        sizer->Add(hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
    }

    m_text = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(600), FromDIP(360)),
                            wxTE_MULTILINE | wxTE_DONTWRAP | (m_mode == Mode::Export ? wxTE_READONLY : 0));
    m_text->SetFont(wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE)));
    sizer->Add(m_text, 1, wxEXPAND | wxALL, FromDIP(10));

    if (m_mode == Mode::Export) {
        auto* btns = new DialogButtons(this, {"Copy", "Cancel"});
        btns->GetButtonFromID(wxID_COPY)->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            wxClipboardLocker lock;
            if (lock)
                wxTheClipboard->SetData(new wxTextDataObject(m_text->GetValue()));
            EndModal(wxID_OK);
        });
        btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        sizer->Add(btns, 0, wxEXPAND);
        update_export_text();
    } else {
        auto* btns = new DialogButtons(this, {"Apply", "Cancel"});
        btns->GetAPPLY()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { apply_import_text(); });
        btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
        sizer->Add(btns, 0, wxEXPAND);
    }

    SetSizerAndFit(sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void SettingsTextDialog::on_dpi_changed(const wxRect& /*suggested_rect*/)
{
    m_text->SetMinSize(wxSize(FromDIP(600), FromDIP(360)));
    GetSizer()->SetSizeHints(this);
    Refresh();
}

void SettingsTextDialog::update_export_text()
{
    const PresetTypeInfo& info    = preset_types[std::max(0, m_type_choice->GetSelection())];
    PresetCollection&     presets = preset_collection(info.type);
    const Preset&         edited  = presets.get_edited_preset();
    const bool            changes_only = m_scope_choice->GetSelection() == 0;

    std::vector<std::string> header{std::string("OrcaSlicer ") + info.name + " settings",
                                    "Printer: " + wxGetApp().preset_bundle->printers.get_edited_preset().name,
                                    "Preset: " + edited.name + (presets.current_is_dirty() ? " (modified)" : "")};
    std::vector<std::string> keys;
    if (changes_only) {
        // The system preset this one inherits from, or the preset itself when it is a system preset.
        const Preset* base = presets.get_selected_preset_parent();
        if (base == nullptr)
            base = &presets.get_selected_preset();
        header.push_back("Changes from: " + base->name);
        keys = edited.config.diff(base->config);
    } else {
        keys = edited.config.keys();
    }
    m_text->SetValue(from_u8(settings_to_text(edited.config, keys, header)));
}

void SettingsTextDialog::apply_import_text()
{
    SettingsTextParseResult  parsed  = settings_from_text(into_u8(m_text->GetValue()));
    std::vector<std::string> skipped = parsed.skipped_keys;
    std::vector<std::string> routed;
    wxString                 applied_summary;

    for (const PresetTypeInfo& info : preset_types) {
        Tab* tab = wxGetApp().get_tab(info.type);
        if (tab == nullptr || tab->get_config() == nullptr)
            continue;
        const DynamicPrintConfig&       current = *tab->get_config();
        const std::vector<std::string>& options = preset_options(info.type);
        DynamicPrintConfig              config;
        for (const std::string& key : parsed.config.keys()) {
            if (std::find(routed.begin(), routed.end(), key) != routed.end() ||
                std::find(options.begin(), options.end(), key) == options.end() || !current.has(key))
                continue;
            routed.push_back(key);
            ConfigOption* opt = parsed.config.option(key)->clone();
            // A single pasted value fills every extruder / variant slot of a vector setting.
            auto* vec     = dynamic_cast<ConfigOptionVectorBase*>(opt);
            auto* cur_vec = dynamic_cast<const ConfigOptionVectorBase*>(current.option(key));
            if (vec != nullptr && cur_vec != nullptr && vec->size() == 1 && cur_vec->size() > 1) {
                ConfigOption* filled     = current.option(key)->clone();
                auto*         filled_vec = dynamic_cast<ConfigOptionVectorBase*>(filled);
                for (size_t i = 0; i < filled_vec->size(); ++i)
                    filled_vec->set_at(opt, i, 0);
                delete opt;
                opt = filled;
            }
            config.set_key_value(key, opt);
        }
        const size_t count = config.keys().size();
        if (count == 0)
            continue;
        tab->load_config(config);
        applied_summary += format_wxstr(_L("%1%: %2% settings"), _L(info.name), count) + "\n";
    }

    for (const std::string& key : parsed.config.keys())
        if (std::find(routed.begin(), routed.end(), key) == routed.end())
            skipped.push_back(key);

    wxString msg = applied_summary.empty() ? _L("No settings were applied.") : _L("Applied") + ":\n" + applied_summary;
    if (!skipped.empty())
        msg += "\n" + format_wxstr(_L("Skipped %1% settings that are unknown or not part of a preset: %2%"), skipped.size(), key_list(skipped));
    if (!parsed.invalid_keys.empty())
        msg += "\n" + format_wxstr(_L("Skipped %1% settings with values that could not be read: %2%"), parsed.invalid_keys.size(),
                                   key_list(parsed.invalid_keys));

    MessageDialog dlg(this, msg, _L("Paste Settings as Text"), wxOK | wxICON_INFORMATION);
    dlg.ShowModal();
    if (!applied_summary.empty())
        EndModal(wxID_OK);
}

}} // namespace Slic3r::GUI
