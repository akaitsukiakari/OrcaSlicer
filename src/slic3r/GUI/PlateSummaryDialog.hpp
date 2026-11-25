#pragma once

#include "GUI_Utils.hpp"

#include <wx/gdicmn.h>
#include <wx/string.h>

class RadioGroup;
class wxTextCtrl;

namespace Slic3r { namespace GUI {

// Copyable text summary of a plate (printer, filaments, unsaved process changes, plate settings, notes and
// each object with its overrides, parts and modifiers), or of one object on its plate.
class PlateSummaryDialog : public DPIDialog
{
public:
    // obj_idx is the selected object, or -1; the Object view is available only with one.
    PlateSummaryDialog(wxWindow* parent, bool object_view, int plate_idx, int obj_idx);

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void update_text();

    int         m_plate_idx;
    int         m_obj_idx;
    // The plate the selected object is on, shown when switching from the Object view to the Plate view.
    int         m_obj_plate_idx;
    RadioGroup* m_view_choice { nullptr };
    wxTextCtrl* m_text { nullptr };
};

}} // namespace Slic3r::GUI
