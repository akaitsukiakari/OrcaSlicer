#include "BookmarksPanel.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/WebView.hpp"
#include "libslic3r/AppConfig.hpp"

#include <nlohmann/json.hpp>

#include <wx/control.h>
#include <wx/cursor.h>
#include <wx/dcbuffer.h>
#include <wx/menu.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/uri.h>
#include <wx/utils.h>
#include <wx/webview.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <utility>

namespace Slic3r { namespace GUI {

namespace {

constexpr const char* KEY_BOOKMARKS = "bookmarks";
constexpr const char* KEY_SELECTED  = "bookmarks_selected";
constexpr const char* KEY_COLLAPSED = "bookmarks_collapsed";

// Light mode colors; StateColor maps them to their dark mode pair.
const wxColour SIDEBAR_BG("#F8F8F8");
const wxColour ROW_HOVER_BG("#EEEEEE");
const wxColour ROW_SELECTED_BG("#009688");
const wxColour ROW_TEXT("#262E30");
const wxColour SEPARATOR("#EEEEEE");

wxColour themed(const wxColour& color) { return StateColor::darkModeColorFor(color); }

} // namespace

// One entry of the sidebar, drawn by hand so the selection and hover colors survive the
// app's dark mode pass. Collapsed, it shows the first letter of the name.
class BookmarkRow : public wxWindow
{
public:
    BookmarkRow(wxWindow* parent, const wxString& name, std::function<void()> on_click, std::function<void()> on_menu)
        : wxWindow(parent, wxID_ANY), m_name(name)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetFont(Label::Body_14);
        SetMinSize(wxSize(-1, FromDIP(34)));
        SetCursor(wxCursor(wxCURSOR_HAND));
        SetToolTip(name);
        Bind(wxEVT_PAINT, &BookmarkRow::on_paint, this);
        Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent&) { m_hovered = true; Refresh(); });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) { m_hovered = false; Refresh(); });
        Bind(wxEVT_LEFT_UP, [on_click](wxMouseEvent&) { on_click(); });
        Bind(wxEVT_RIGHT_UP, [on_menu](wxMouseEvent&) { on_menu(); });
    }

    void set_selected(bool selected) { m_selected = selected; Refresh(); }
    void set_collapsed(bool collapsed) { m_collapsed = collapsed; Refresh(); }

private:
    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        const wxSize size = GetClientSize();
        dc.SetBackground(wxBrush(themed(SIDEBAR_BG)));
        dc.Clear();
        if (m_selected || m_hovered) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(themed(m_selected ? ROW_SELECTED_BG : ROW_HOVER_BG)));
            dc.DrawRoundedRectangle(FromDIP(4), FromDIP(2), size.x - FromDIP(8), size.y - FromDIP(4), FromDIP(4));
        }
        dc.SetFont(GetFont());
        dc.SetTextForeground(m_selected ? *wxWHITE : themed(ROW_TEXT));
        if (m_collapsed) {
            const wxString letter = m_name.Left(1).Upper();
            const wxSize   extent = dc.GetTextExtent(letter);
            dc.DrawText(letter, (size.x - extent.x) / 2, (size.y - extent.y) / 2);
        } else {
            const int      pad  = FromDIP(12);
            const wxString text = wxControl::Ellipsize(m_name, dc, wxELLIPSIZE_END, size.x - 2 * pad);
            dc.DrawText(text, pad, (size.y - dc.GetTextExtent(text).y) / 2);
        }
    }

    wxString m_name;
    bool     m_selected{false};
    bool     m_hovered{false};
    bool     m_collapsed{false};
};

BookmarksPanel::BookmarksPanel(wxWindow* parent) : wxPanel(parent, wxID_ANY)
{
    load_bookmarks();

    m_sidebar = new wxPanel(this);
    m_sidebar->SetBackgroundColour(SIDEBAR_BG);
    auto sidebar_sizer = new wxBoxSizer(wxVERTICAL);

    auto header  = new wxBoxSizer(wxHORIZONTAL);
    m_title      = new Label(m_sidebar, Label::Head_14, _L("Bookmarks"));
    m_title->SetBackgroundColour(SIDEBAR_BG);
    m_collapse_btn = new Button(m_sidebar, "");
    m_collapse_btn->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    m_collapse_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        m_collapsed = !m_collapsed;
        wxGetApp().app_config->set("app", KEY_COLLAPSED, m_collapsed);
        update_collapsed();
    });
    header->Add(m_title, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    header->Add(m_collapse_btn, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(4));
    sidebar_sizer->Add(header, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(4));

    m_list_sizer = new wxBoxSizer(wxVERTICAL);
    sidebar_sizer->Add(m_list_sizer, 1, wxEXPAND);

    m_footer = new wxBoxSizer(wxHORIZONTAL);
    auto add_btn = new Button(m_sidebar, wxEmptyString, "add", 0, 16);
    add_btn->SetStyle(ButtonStyle::Regular, ButtonType::Icon);
    add_btn->SetToolTip(_L("Add bookmark"));
    add_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { add_bookmark(); });
    m_reload_btn = new Button(m_sidebar, wxEmptyString, "refresh", 0, 16);
    m_reload_btn->SetStyle(ButtonStyle::Regular, ButtonType::Icon);
    m_reload_btn->SetToolTip(_L("Reload page"));
    m_reload_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (m_browser != nullptr)
            m_browser->Reload();
    });
    m_footer->Add(add_btn, 0, wxALL, FromDIP(4));
    m_footer->Add(m_reload_btn, 0, wxALL, FromDIP(4));
    sidebar_sizer->Add(m_footer, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(4));
    m_sidebar->SetSizer(sidebar_sizer);

    auto separator = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(1), -1));
    separator->SetBackgroundColour(SEPARATOR);

    m_empty_hint = new Label(this, _L("No bookmarks yet. Use the + button to add a web page, such as Spoolman or a camera stream."));
    m_empty_hint->Wrap(FromDIP(500));
    m_browser    = WebView::CreateWebView(this, "");
    if (m_browser != nullptr) {
        // Pages that open a new window (target="_blank" links) stay in the tab.
        m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, [this](wxWebViewEvent& evt) {
            if (!evt.GetURL().empty())
                m_browser->LoadURL(evt.GetURL());
            evt.Veto();
        });
    }

    auto content = new wxBoxSizer(wxVERTICAL);
    content->Add(m_empty_hint, 0, wxALL, FromDIP(20));
    if (m_browser != nullptr)
        content->Add(m_browser, 1, wxEXPAND);

    auto main_sizer = new wxBoxSizer(wxHORIZONTAL);
    main_sizer->Add(m_sidebar, 0, wxEXPAND);
    main_sizer->Add(separator, 0, wxEXPAND);
    main_sizer->Add(content, 1, wxEXPAND);
    SetSizer(main_sizer);

    rebuild_list();
    update_collapsed();
}

BookmarksPanel::~BookmarksPanel()
{
    if (m_browser != nullptr) {
        m_browser->Destroy();
        m_browser = nullptr;
    }
}

bool BookmarksPanel::Show(bool show)
{
    const bool changed = wxPanel::Show(show);
    if (show && !m_loaded) {
        m_loaded = true;
        select(m_selected);
    }
    return changed;
}

void BookmarksPanel::load_bookmarks()
{
    AppConfig* config = wxGetApp().app_config;
    m_bookmarks.clear();
    try {
        const nlohmann::json list = nlohmann::json::parse(config->get(KEY_BOOKMARKS), nullptr, false);
        if (list.is_array())
            for (const nlohmann::json& item : list)
                if (item.is_object() && item.value("url", "") != "")
                    m_bookmarks.push_back({item.value("name", ""), item.value("url", "")});
    } catch (const std::exception&) {
        // A malformed entry leaves the list as read so far.
    }
    m_collapsed = config->get_bool(KEY_COLLAPSED);
    const std::string selected = config->get(KEY_SELECTED);
    m_selected = selected.empty() ? 0 : std::atoi(selected.c_str());
    if (m_selected < 0 || m_selected >= int(m_bookmarks.size()))
        m_selected = m_bookmarks.empty() ? -1 : 0;
}

void BookmarksPanel::save_bookmarks() const
{
    nlohmann::json list = nlohmann::json::array();
    for (const Bookmark& bookmark : m_bookmarks)
        list.push_back({{"name", bookmark.name}, {"url", bookmark.url}});
    AppConfig* config = wxGetApp().app_config;
    config->set("app", KEY_BOOKMARKS, list.dump());
    config->set("app", KEY_SELECTED, std::to_string(m_selected));
}

void BookmarksPanel::rebuild_list()
{
    m_list_sizer->Clear(true);
    m_rows.clear();
    for (size_t i = 0; i < m_bookmarks.size(); ++i) {
        const int idx = int(i);
        auto      row = new BookmarkRow(m_sidebar, from_u8(m_bookmarks[i].name), [this, idx]() { select(idx); },
                                        // After the click handler returns: the menu can rebuild the rows.
                                        [this, idx]() { CallAfter([this, idx]() { show_row_menu(idx); }); });
        row->set_selected(idx == m_selected);
        row->set_collapsed(m_collapsed);
        m_list_sizer->Add(row, 0, wxEXPAND);
        m_rows.push_back(row);
    }
    const bool empty = m_bookmarks.empty();
    m_empty_hint->Show(empty);
    if (m_browser != nullptr)
        m_browser->Show(!empty);
    m_reload_btn->Show(!empty);
    m_sidebar->Layout();
    Layout();
}

void BookmarksPanel::update_collapsed()
{
    m_title->Show(!m_collapsed);
    // Too narrow collapsed for the buttons side by side.
    m_footer->SetOrientation(m_collapsed ? wxVERTICAL : wxHORIZONTAL);
    m_collapse_btn->SetLabel(m_collapsed ? wxString::FromUTF8("\xC2\xBB") : wxString::FromUTF8("\xC2\xAB"));
    m_collapse_btn->SetToolTip(m_collapsed ? _L("Expand") : _L("Collapse"));
    for (BookmarkRow* row : m_rows)
        row->set_collapsed(m_collapsed);
    const int width = FromDIP(m_collapsed ? 44 : 200);
    m_sidebar->SetMinSize(wxSize(width, -1));
    m_sidebar->SetMaxSize(wxSize(width, -1));
    m_sidebar->Layout();
    Layout();
}

void BookmarksPanel::select(int idx)
{
    if (idx < 0 || idx >= int(m_bookmarks.size()))
        return;
    m_selected = idx;
    for (size_t i = 0; i < m_rows.size(); ++i)
        m_rows[i]->set_selected(int(i) == idx);
    wxGetApp().app_config->set("app", KEY_SELECTED, std::to_string(idx));
    // Clicking the selected bookmark again goes back to its page.
    if (m_browser != nullptr && m_loaded)
        m_browser->LoadURL(from_u8(m_bookmarks[idx].url));
}

void BookmarksPanel::show_row_menu(int idx)
{
    enum { ID_EDIT = wxID_HIGHEST + 1, ID_REMOVE, ID_UP, ID_DOWN, ID_BROWSER };
    wxMenu menu;
    menu.Append(ID_EDIT, _L("Edit") + dots);
    menu.Append(ID_REMOVE, _L("Remove"));
    menu.AppendSeparator();
    menu.Append(ID_UP, _L("Move up"))->Enable(idx > 0);
    menu.Append(ID_DOWN, _L("Move down"))->Enable(idx + 1 < int(m_bookmarks.size()));
    menu.AppendSeparator();
    menu.Append(ID_BROWSER, _L("Open in browser"));
    switch (GetPopupMenuSelectionFromUser(menu)) {
    case ID_EDIT: edit_bookmark(idx); break;
    case ID_REMOVE: remove_bookmark(idx); break;
    case ID_UP: move_bookmark(idx, -1); break;
    case ID_DOWN: move_bookmark(idx, 1); break;
    case ID_BROWSER: wxLaunchDefaultBrowser(from_u8(m_bookmarks[idx].url)); break;
    default: break;
    }
}

void BookmarksPanel::add_bookmark()
{
    BookmarkDialog dlg(this, _L("Add bookmark"), {});
    if (dlg.ShowModal() != wxID_OK)
        return;
    m_bookmarks.push_back(dlg.bookmark());
    m_selected = int(m_bookmarks.size()) - 1;
    save_bookmarks();
    rebuild_list();
    select(m_selected);
}

void BookmarksPanel::edit_bookmark(int idx)
{
    BookmarkDialog dlg(this, _L("Edit bookmark"), m_bookmarks[idx]);
    if (dlg.ShowModal() != wxID_OK)
        return;
    const bool url_changed = dlg.bookmark().url != m_bookmarks[idx].url;
    m_bookmarks[idx]       = dlg.bookmark();
    save_bookmarks();
    rebuild_list();
    if (url_changed && idx == m_selected)
        select(idx);
}

void BookmarksPanel::remove_bookmark(int idx)
{
    MessageDialog msg(this, wxString::Format(_L("Remove the bookmark \"%s\"?"), from_u8(m_bookmarks[idx].name)), _L("Remove bookmark"),
                      wxYES_NO | wxICON_QUESTION);
    if (msg.ShowModal() != wxID_YES)
        return;
    m_bookmarks.erase(m_bookmarks.begin() + idx);
    const bool was_selected = idx == m_selected;
    if (m_selected > idx || m_selected >= int(m_bookmarks.size()))
        --m_selected;
    save_bookmarks();
    rebuild_list();
    if (m_bookmarks.empty()) {
        if (m_browser != nullptr)
            m_browser->LoadURL("about:blank");
    } else if (was_selected) {
        select(std::max(m_selected, 0));
    }
}

void BookmarksPanel::move_bookmark(int idx, int offset)
{
    const int other = idx + offset;
    if (other < 0 || other >= int(m_bookmarks.size()))
        return;
    std::swap(m_bookmarks[idx], m_bookmarks[other]);
    if (m_selected == idx)
        m_selected = other;
    else if (m_selected == other)
        m_selected = idx;
    save_bookmarks();
    rebuild_list();
}

BookmarkDialog::BookmarkDialog(wxWindow* parent, const wxString& title, const BookmarksPanel::Bookmark& initial)
    : DPIDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
    SetBackgroundColour(*wxWHITE);
    auto main_sizer = new wxBoxSizer(wxVERTICAL);

    auto grid = new wxFlexGridSizer(2, FromDIP(10), FromDIP(12));
    grid->AddGrowableCol(1);
    m_name = new TextInput(this, from_u8(initial.name), "", "", wxDefaultPosition, wxSize(FromDIP(360), -1));
    m_url  = new TextInput(this, from_u8(initial.url), "", "", wxDefaultPosition, wxSize(FromDIP(360), -1));
    m_url->GetTextCtrl()->SetHint("http://192.168.0.10:7912");
    grid->Add(new Label(this, _L("Name")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_name, 1, wxEXPAND);
    grid->Add(new Label(this, _L("URL")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(m_url, 1, wxEXPAND);
    main_sizer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(20));

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (bookmark().url.empty()) {
            MessageDialog msg(this, _L("Enter the address of the page."), GetTitle(), wxOK);
            msg.ShowModal();
            return;
        }
        EndModal(wxID_OK);
    });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    main_sizer->Add(dlg_btns, 0, wxEXPAND);

    SetSizerAndFit(main_sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

BookmarksPanel::Bookmark BookmarkDialog::bookmark() const
{
    std::string url = into_u8(m_url->GetTextCtrl()->GetValue().Strip(wxString::both));
    if (!url.empty() && url.find("://") == std::string::npos)
        url = "http://" + url;
    std::string name = into_u8(m_name->GetTextCtrl()->GetValue().Strip(wxString::both));
    // Unnamed, the bookmark shows its host.
    if (name.empty() && !url.empty())
        name = into_u8(wxURI(from_u8(url)).GetServer());
    if (name.empty())
        name = url;
    return {name, url};
}

void BookmarkDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    Fit();
    Refresh();
}

}} // namespace Slic3r::GUI
