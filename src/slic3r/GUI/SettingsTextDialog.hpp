#pragma once

#include "GUI_Utils.hpp"

#include <vector>

#include <wx/string.h>

class RadioGroup;
class wxGrid;
class wxTextCtrl;

namespace Slic3r { namespace GUI {

// Copies the process, filament or printer settings as "key = value" text, or applies pasted text to the
// current presets as unsaved changes. The text format is defined in libslic3r/SettingsText.hpp.
class SettingsTextDialog : public DPIDialog
{
public:
    enum class Mode { Export, Import };

    SettingsTextDialog(wxWindow* parent, Mode mode);

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void update_export_text();
    // Parses the text and reports what it would apply; applies it to the presets when apply is true.
    void import_text(bool apply);
    // The text as shown, or rebuilt from the table when the table view is shown.
    wxString current_text() const;
    void     fill_table(const wxString& text);
    void     show_table(bool table);

    Mode        m_mode;
    RadioGroup* m_type_choice{nullptr};
    RadioGroup* m_scope_choice{nullptr};
    RadioGroup* m_view_choice{nullptr};
    wxTextCtrl* m_text{nullptr};
    wxGrid*     m_grid{nullptr};
    // Comment lines of the text, kept while the table view (which has no place for them) is shown.
    std::vector<wxString> m_table_comments;
};

}} // namespace Slic3r::GUI
