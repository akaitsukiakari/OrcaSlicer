#pragma once

#include "GUI_Utils.hpp"

class RadioGroup;
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
    void apply_import_text();

    Mode        m_mode;
    RadioGroup* m_type_choice{nullptr};
    RadioGroup* m_scope_choice{nullptr};
    wxTextCtrl* m_text{nullptr};
};

}} // namespace Slic3r::GUI
