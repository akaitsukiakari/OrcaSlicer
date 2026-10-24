#include "SpoolmanSpoolDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "NotificationManager.hpp"
#include "Plater.hpp"
#include "PresetComboBoxes.hpp"
#include "SpoolmanLinkDialog.hpp"
#include "format.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/algorithm/string/predicate.hpp>

#include <wx/bitmap.h>
#include <wx/utils.h>
#include <wx/dcmemory.h>
#include <wx/imaglist.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>

#include <string>
#include <utility>

namespace Slic3r { namespace GUI {

namespace {

// A square of the spool's colors, side by side for a multicolor filament.
wxBitmap color_swatch(const std::vector<std::string>& colors, int size)
{
    wxBitmap   bitmap(size, size);
    wxMemoryDC dc(bitmap);
    dc.SetBackground(*wxWHITE_BRUSH);
    dc.Clear();
    dc.SetPen(*wxTRANSPARENT_PEN);
    const int count = int(colors.size());
    for (int i = 0; i < count; ++i) {
        dc.SetBrush(wxBrush(wxColour(from_u8(colors[i]))));
        dc.DrawRectangle(size * i / count, 0, size * (i + 1) / count - size * i / count, size);
    }
    dc.SetPen(wxPen(wxColour(172, 172, 172)));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRectangle(0, 0, size, size);
    dc.SelectObject(wxNullBitmap);
    return bitmap;
}

} // anonymous namespace

SpoolmanSpoolDialog::SpoolmanSpoolDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Choose a Spoolman spool"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetBackgroundColour(*wxWHITE);
    auto main_sizer = new wxBoxSizer(wxVERTICAL);

    auto address_sizer = new wxBoxSizer(wxHORIZONTAL);
    address_sizer->Add(new Label(this, _L("Spoolman address")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    m_address = new TextInput(this, from_u8(wxGetApp().app_config->get(Spoolman::URL_KEY)), "", "", wxDefaultPosition,
                              wxSize(FromDIP(260), -1), wxTE_PROCESS_ENTER);
    m_address->GetTextCtrl()->SetHint("http://192.168.0.10:7912");
    m_address->SetToolTip(_L("Leave it empty to read Spoolman through the printer that was last synced, if it uses Spoolman."));
    address_sizer->Add(m_address, 1, wxEXPAND | wxRIGHT, FromDIP(8));
    auto load_btn = new Button(this, _L("Refresh"));
    load_btn->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
    address_sizer->Add(load_btn, 0, wxALIGN_CENTER_VERTICAL);
    main_sizer->Add(address_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(20));

    m_search = new TextInput(this, "", "", "search", wxDefaultPosition, wxDefaultSize);
    m_search->GetTextCtrl()->SetHint(_L("Search"));
    main_sizer->Add(m_search, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_status = new Label(this, "");
    main_sizer->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(560), FromDIP(360)), wxLC_REPORT | wxLC_SINGLE_SEL);
    m_list->AppendColumn(_L("Spool"), wxLIST_FORMAT_LEFT, FromDIP(300));
    m_list->AppendColumn(_L("Remaining"), wxLIST_FORMAT_RIGHT, FromDIP(90));
    m_list->AppendColumn(_L("Location"), wxLIST_FORMAT_LEFT, FromDIP(110));
    m_list->AppendColumn(_L("ID"), wxLIST_FORMAT_RIGHT, FromDIP(50));
    main_sizer->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    auto buttons_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto custom_btn    = new Button(this, _L("Custom color"));
    custom_btn->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    custom_btn->SetToolTip(_L("Choose a color without a Spoolman spool."));
    buttons_sizer->Add(custom_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(20));
    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    buttons_sizer->Add(dlg_btns, 1, wxEXPAND);
    main_sizer->Add(buttons_sizer, 0, wxEXPAND | wxTOP, FromDIP(10));

    auto choose = [this]() {
        if (selected())
            EndModal(wxID_OK);
    };
    auto reload = [this]() {
        wxGetApp().app_config->set(Spoolman::URL_KEY, into_u8(m_address->GetTextCtrl()->GetValue()));
        load();
    };
    load_btn->Bind(wxEVT_BUTTON, [reload](wxCommandEvent&) { reload(); });
    m_address->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [reload](wxCommandEvent&) { reload(); });
    m_search->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { fill_list(); });
    m_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [choose](wxListEvent&) { choose(); });
    custom_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(ID_CUSTOM_COLOR); });
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [choose](wxCommandEvent&) { choose(); });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });

    SetSizerAndFit(main_sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);

    load();
    m_search->GetTextCtrl()->SetFocus();
}

const SpoolmanSpool* SpoolmanSpoolDialog::selected() const
{
    const long row = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    return row >= 0 && row < long(m_rows.size()) ? &m_spools[m_rows[row]] : nullptr;
}

void SpoolmanSpoolDialog::load()
{
    std::string error;
    bool        read;
    {
        wxBusyCursor busy;
        read = Spoolman::fetch_spools(m_spools, error);
    }
    if (!read)
        m_status->SetLabel(error.empty() ? _L("Enter the Spoolman address, or sync the filaments of a printer that uses Spoolman.") :
                                           // TRN %1% is the error, in English.
                                           format_wxstr(_L("Spoolman could not be read: %1%"), error));
    else if (m_spools.empty())
        m_status->SetLabel(_L("Spoolman has no spools."));
    else
        m_status->SetLabel("");
    m_status->Show(!m_status->GetLabel().empty());
    fill_list();
    Layout();
}

void SpoolmanSpoolDialog::fill_list()
{
    const std::string search = into_u8(m_search->GetTextCtrl()->GetValue());
    const int         size   = FromDIP(16);
    auto              images = new wxImageList(size, size);
    m_list->DeleteAllItems();
    m_list->AssignImageList(images, wxIMAGE_LIST_SMALL);
    m_rows.clear();
    for (size_t i = 0; i < m_spools.size(); ++i) {
        const SpoolmanSpool& spool = m_spools[i];
        const std::string    label = spool.label();
        if (!search.empty() && !boost::algorithm::icontains(label, search) && !boost::algorithm::icontains(spool.location, search) &&
            std::to_string(spool.id) != search)
            continue;
        const long row = m_list->InsertItem(long(m_rows.size()), from_u8(label), images->Add(color_swatch(spool.colors, size)));
        if (spool.initial_weight > 0.)
            m_list->SetItem(row, 1, wxString::Format("%d g", int(spool.remaining_weight)));
        m_list->SetItem(row, 2, from_u8(spool.location));
        m_list->SetItem(row, 3, wxString::Format("%d", spool.id));
        m_rows.push_back(i);
    }
    if (m_rows.size() == 1)
        m_list->SetItemState(0, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
}

void SpoolmanSpoolDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    fill_list();
    Fit();
    Refresh();
}

void pick_spoolman_spool(PlaterPresetComboBox& combo)
{
    SpoolmanSpool spool;
    {
        SpoolmanSpoolDialog dlg(wxGetApp().mainframe);
        const int           result = dlg.ShowModal();
        if (result == SpoolmanSpoolDialog::ID_CUSTOM_COLOR)
            combo.show_default_color_picker();
        if (result != wxID_OK || !dlg.selected())
            return;
        spool = *dlg.selected();
    }

    PresetBundle& bundle = *wxGetApp().preset_bundle;
    const int     idx    = combo.get_filament_idx();
    if (idx < 0 || idx >= int(bundle.filament_presets.size()))
        return;

    if (Spoolman::linked_preset(spool.filament_id).empty()) {
        SpoolmanLinkDialog link(wxGetApp().mainframe,
                                _L("Choose the filament preset to use for this Spoolman filament. Choosing a spool of it, or syncing a "
                                   "printer slot that holds one, then selects its preset. Change the links in Preferences."),
                                {{spool.filament_id, spool.label(), bundle.filament_presets[idx]}});
        if (link.ShowModal() == wxID_OK)
            link.save_links();
    }
    const std::string linked = Spoolman::linked_preset(spool.filament_id);
    if (!linked.empty() && linked != Spoolman::NOT_LINKED && linked != bundle.filament_presets[idx] && !combo.select_preset(linked)) {
        // TRN %1% is a filament preset, %2% the Spoolman filament name.
        MessageDialog msg(wxGetApp().mainframe, format_wxstr(_L("The preset \"%1%\" linked to %2% can't be used with the selected printer."), linked, spool.label()),
                          _L("Choose a Spoolman spool"), wxOK | wxICON_WARNING);
        msg.ShowModal();
    }

    const std::string preset = bundle.filament_presets[idx];
    if (linked == preset && Spoolman::update_preset(bundle.filaments, preset, spool))
        wxGetApp().plater()->get_notification_manager()->push_notification(
            NotificationType::CustomNotification, NotificationManager::NotificationLevel::RegularNotificationLevel,
            into_u8(_L("Cost and density updated from Spoolman:") + "\n- " + from_u8(preset)));
    // Checked against the remaining weight when slicing, as a synced spool is.
    Spoolman::synced_filaments()[idx] = {spool, preset};
    if (!spool.colors.empty())
        combo.sync_colour_config(spool.colors, spool.gradient);
}

}} // namespace Slic3r::GUI
