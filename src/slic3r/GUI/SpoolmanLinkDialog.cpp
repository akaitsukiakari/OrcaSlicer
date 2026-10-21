#include "SpoolmanLinkDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/Spoolman.hpp"

#include <wx/scrolwin.h>
#include <wx/sizer.h>

#include <algorithm>
#include <set>
#include <utility>

namespace Slic3r { namespace GUI {

SpoolmanLinkDialog::SpoolmanLinkDialog(wxWindow* parent, const wxString& message, std::vector<Row> rows)
    : DPIDialog(parent, wxID_ANY, _L("Link Spoolman filaments"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_rows(std::move(rows))
{
    SetBackgroundColour(*wxWHITE);

    // The user's own presets first, then the system ones.
    const PresetCollection& filaments = wxGetApp().preset_bundle->filaments;
    for (bool system : {false, true})
        for (const Preset& preset : filaments.get_presets())
            if (preset.is_visible && preset.is_compatible && !preset.is_default && preset.is_system == system)
                m_presets.push_back(preset.name);

    auto main_sizer = new wxBoxSizer(wxVERTICAL);
    auto text       = new Label(this, message);
    text->Wrap(FromDIP(460));
    main_sizer->Add(text, 0, wxALL, FromDIP(20));

    auto scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    scroll->SetBackgroundColour(*wxWHITE);
    scroll->SetScrollRate(0, FromDIP(10));
    auto grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(12));
    grid->AddGrowableCol(1);
    for (const Row& row : m_rows) {
        grid->Add(new Label(scroll, from_u8(row.label)), 0, wxALIGN_CENTER_VERTICAL);
        auto combo = new ComboBox(scroll, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(300), FromDIP(24)), 0, nullptr, wxCB_READONLY);
        combo->Append(_L("Not linked"));
        int selection = 0;
        for (size_t i = 0; i < m_presets.size(); ++i) {
            combo->Append(from_u8(m_presets[i]));
            if (m_presets[i] == row.preset)
                selection = int(i) + 1;
        }
        combo->SetSelection(selection);
        grid->Add(combo, 1, wxEXPAND);
        m_combos.push_back(combo);
    }
    scroll->SetSizer(grid);
    grid->FitInside(scroll);
    scroll->SetMinSize(wxSize(-1, std::min(grid->GetMinSize().GetHeight(), FromDIP(360))));
    main_sizer->Add(scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    main_sizer->Add(dlg_btns, 0, wxEXPAND | wxTOP, FromDIP(10));

    SetSizerAndFit(main_sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void SpoolmanLinkDialog::save_links() const
{
    for (size_t i = 0; i < m_rows.size(); ++i) {
        const int selection = m_combos[i]->GetSelection();
        Spoolman::set_linked_preset(m_rows[i].filament_id, m_rows[i].label,
                                    selection > 0 ? m_presets[selection - 1] : Spoolman::NOT_LINKED);
    }
}

void SpoolmanLinkDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    Fit();
    Refresh();
}

void link_new_spoolman_filaments(wxWindow* parent)
{
    std::vector<SpoolmanLinkDialog::Row> rows;
    std::set<int>                        filament_ids;
    for (const auto& [slot, spool] : Spoolman::slot_spools())
        if (Spoolman::linked_preset(spool.filament_id).empty() && filament_ids.insert(spool.filament_id).second)
            rows.push_back({spool.filament_id, spool.label(), ""});
    if (rows.empty())
        return;

    SpoolmanLinkDialog dlg(parent,
                           _L("Choose the filament preset to use for each Spoolman filament loaded in the printer. "
                              "Syncing a slot that holds a spool of a linked filament then selects its preset. "
                              "You won't be asked again about a filament left unlinked; change the links in Preferences."),
                           std::move(rows));
    if (dlg.ShowModal() == wxID_OK)
        dlg.save_links();
}

void edit_spoolman_links(wxWindow* parent)
{
    std::vector<SpoolmanLinkDialog::Row> rows;
    for (const auto& [filament_id, link] : Spoolman::links())
        rows.push_back({filament_id, link.first, link.second});
    if (rows.empty()) {
        MessageDialog msg(parent, _L("No Spoolman filaments have been linked yet. You are asked to link them when you sync the filaments of a printer that uses Spoolman."),
                          _L("Link Spoolman filaments"), wxOK);
        msg.ShowModal();
        return;
    }
    SpoolmanLinkDialog dlg(parent, _L("Choose the filament preset to use for each Spoolman filament."), std::move(rows));
    if (dlg.ShowModal() == wxID_OK)
        dlg.save_links();
}

}} // namespace Slic3r::GUI
