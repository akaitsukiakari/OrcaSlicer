#pragma once

#include <string>
#include <vector>

#include <wx/panel.h>
#include <wx/string.h>

#include "GUI_Utils.hpp"

class wxBoxSizer;
class wxWebView;
class Button;
class Label;
class TextInput;

namespace Slic3r { namespace GUI {

class BookmarkRow;

// The Bookmarks tab: a sidebar of user-saved web pages (Spoolman, Fluidd pages, a camera
// stream...) beside one web view that shows the selected page. The list is kept in the app
// config as JSON under "bookmarks"; the sidebar collapses to the first letter of each name.
class BookmarksPanel : public wxPanel
{
public:
    struct Bookmark
    {
        std::string name;
        std::string url;
    };

    explicit BookmarksPanel(wxWindow* parent);
    ~BookmarksPanel() override;

    // Loads the remembered page the first time the tab is shown.
    bool Show(bool show = true) override;

private:
    void load_bookmarks();
    void save_bookmarks() const;
    void rebuild_list();
    void update_collapsed();
    void select(int idx);
    void show_row_menu(int idx);
    void add_bookmark();
    void edit_bookmark(int idx);
    void remove_bookmark(int idx);
    void move_bookmark(int idx, int offset);

    std::vector<Bookmark>     m_bookmarks;
    std::vector<BookmarkRow*> m_rows;
    int                       m_selected{-1};
    bool                      m_collapsed{false};
    bool                      m_loaded{false};

    wxPanel*    m_sidebar{nullptr};
    wxBoxSizer* m_list_sizer{nullptr};
    wxBoxSizer* m_footer{nullptr};
    Label*      m_title{nullptr};
    Button*     m_collapse_btn{nullptr};
    Button*     m_reload_btn{nullptr};
    Label*      m_empty_hint{nullptr};
    wxWebView*  m_browser{nullptr};
};

// Name and address of one bookmark.
class BookmarkDialog : public DPIDialog
{
public:
    BookmarkDialog(wxWindow* parent, const wxString& title, const BookmarksPanel::Bookmark& initial);

    // The entered bookmark; a URL without a scheme gets "http://".
    BookmarksPanel::Bookmark bookmark() const;

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    TextInput* m_name{nullptr};
    TextInput* m_url{nullptr};
};

}} // namespace Slic3r::GUI
