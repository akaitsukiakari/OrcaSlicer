#pragma once

#include "GUI_Utils.hpp"

#include <string>
#include <vector>

class ComboBox;
class wxWindow;

namespace Slic3r { namespace GUI {

// Links Spoolman filaments to filament presets, so syncing a printer slot that holds a spool of
// that filament selects the linked preset.
class SpoolmanLinkDialog : public DPIDialog
{
public:
    struct Row
    {
        int         filament_id = 0;
        std::string label;
        std::string preset; // empty or Spoolman::NOT_LINKED when not linked
    };

    SpoolmanLinkDialog(wxWindow* parent, const wxString& message, std::vector<Row> rows);

    // Saves the chosen links. A row left unlinked is saved as Spoolman::NOT_LINKED, so it isn't asked again.
    void save_links() const;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    std::vector<Row>         m_rows;
    std::vector<ComboBox*>   m_combos;
    std::vector<std::string> m_presets; // m_presets[i] is item i + 1 of every combo
};

// Asks the user to link the filaments of the spools in the printer slots that they haven't been
// asked about yet. Cancelling asks again at the next sync.
void link_new_spoolman_filaments(wxWindow* parent);

// Lets the user change every link they made.
void edit_spoolman_links(wxWindow* parent);

}} // namespace Slic3r::GUI
