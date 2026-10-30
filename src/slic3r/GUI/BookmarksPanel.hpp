#pragma once

#include <string>
#include <vector>

#include <wx/panel.h>
#include <wx/string.h>
#include <wx/timer.h>

#include "GUI_Utils.hpp"

class wxBoxSizer;
class wxWebView;
class wxIconizeEvent;
class wxTopLevelWindow;
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

    // Loads the selected page when the tab is shown; a hidden tab unloads it after a delay.
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
    // Load the selected page if it isn't, or unload it. An unloaded page can't keep the display awake.
    void resume();
    void suspend();
    void update_view();
    void on_iconize(wxIconizeEvent& evt);

    std::vector<Bookmark>     m_bookmarks;
    std::vector<BookmarkRow*> m_rows;
    int                       m_selected{-1};
    bool                      m_collapsed{false};
    // The selected page is loaded in m_browser.
    bool                      m_active{false};
    // Unloads the page a while after the tab is hidden or the window minimized.
    wxTimer                   m_unload_timer;
    wxTopLevelWindow*         m_frame{nullptr};

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
