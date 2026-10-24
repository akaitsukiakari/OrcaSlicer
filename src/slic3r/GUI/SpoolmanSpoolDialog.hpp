#pragma once

#include "GUI_Utils.hpp"
#include "slic3r/Utils/Spoolman.hpp"

#include <wx/defs.h>

#include <vector>

class Label;
class TextInput;
class wxListCtrl;
class wxWindow;

namespace Slic3r { namespace GUI {

class PlaterPresetComboBox;

// Lists the Spoolman spools to choose the one a plater filament uses.
class SpoolmanSpoolDialog : public DPIDialog
{
public:
    // Ends the dialog when the user asks for a color of their own instead of a spool.
    static constexpr int ID_CUSTOM_COLOR = wxID_HIGHEST + 1;

    explicit SpoolmanSpoolDialog(wxWindow* parent);

    // The chosen spool, nullptr when none is selected.
    const SpoolmanSpool* selected() const;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    // Reads the spools from Spoolman.
    void load();
    // Lists the spools matching the search text.
    void fill_list();

    TextInput*                 m_address = nullptr;
    TextInput*                 m_search  = nullptr;
    wxListCtrl*                m_list    = nullptr;
    Label*                     m_status  = nullptr;
    std::vector<SpoolmanSpool> m_spools;
    std::vector<size_t>        m_rows; // m_rows[i] is the index in m_spools of list row i
};

// Lets the user choose a Spoolman spool for the filament of combo. Selects the preset linked to the
// spool's filament, asking to link it first, and takes the spool's color, cost and density, as
// syncing a printer slot holding that spool does.
void pick_spoolman_spool(PlaterPresetComboBox& combo);

}} // namespace Slic3r::GUI
