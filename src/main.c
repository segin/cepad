#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdlib.h>
#include <string.h>
#include "resource.h"

#define APP_CLASS_NAME TEXT("CEPadWindow")
#define FIND_CLASS_NAME TEXT("CEPadFindWindow")
#define APP_TITLE TEXT("CE Pad")
#define IDC_TAB 1001
#define FIND_BUFFER_LENGTH 128
#define APP_MAX_FILE_SIZE (16 * 1024 * 1024)

typedef struct DOCUMENT_TAG {
    HWND edit;
    WCHAR path[MAX_PATH];
    unsigned int untitled_id;
    int dirty;
} DOCUMENT;

typedef struct APP_STATE_TAG {
    HINSTANCE instance;
    HWND window;
    HWND command_bar;
    HWND tab;
    HMENU menu;
    HACCEL accelerator;
    HFONT ui_font;
    HFONT edit_font;
    LOGFONT edit_logfont;
    int has_custom_font;
    int owns_edit_font;
    DOCUMENT* docs;
    size_t doc_count;
    size_t doc_capacity;
    int active_doc;
    int word_wrap;
    unsigned int next_untitled_id;
    WCHAR find_buffer[FIND_BUFFER_LENGTH];
    int find_match_case;
    int find_reverse;
    HWND find_window;
    HWND find_edit;
    HWND find_match_case_box;
    HWND find_up_radio;
    HWND find_down_radio;
} APP_STATE;

static WNDPROC g_edit_wndproc = NULL;
static WNDPROC g_tab_wndproc = NULL;

static APP_STATE* App_GetState(HWND window) {
    return (APP_STATE*)GetWindowLongPtr(window, GWLP_USERDATA);
}

static HFONT App_GetUiFont(void) {
    return (HFONT)GetStockObject(SYSTEM_FONT);
}

static int App_GetDpiY(void) {
    HDC screen_dc;
    int dpi_y;

    screen_dc = GetDC(NULL);
    dpi_y = screen_dc ? GetDeviceCaps(screen_dc, LOGPIXELSY) : 96;
    if (screen_dc) {
        ReleaseDC(NULL, screen_dc);
    }

    if (dpi_y <= 0) {
        dpi_y = 96;
    }

    return dpi_y;
}

static int App_MulDivRound(int number, int numerator, int denominator) {
    if (denominator == 0) {
        return 0;
    }

    if (number >= 0) {
        return (number * numerator + (denominator / 2)) / denominator;
    }

    return -(((-number) * numerator + (denominator / 2)) / denominator);
}

static HFONT App_CreateDefaultEditFont(LOGFONT* logfont_out) {
    LOGFONT logfont;

    memset(&logfont, 0, sizeof(logfont));
    logfont.lfHeight = -App_MulDivRound(10, App_GetDpiY(), 72);
    logfont.lfWeight = FW_NORMAL;
    logfont.lfCharSet = DEFAULT_CHARSET;
    lstrcpyn(logfont.lfFaceName, TEXT("Courier New"), LF_FACESIZE);

    if (logfont_out) {
        memcpy(logfont_out, &logfont, sizeof(logfont));
    }

    return CreateFontIndirect(&logfont);
}

static const WCHAR* App_GetBaseName(const WCHAR* path) {
    const WCHAR* cursor;
    const WCHAR* base;

    if (!path || !path[0]) {
        return TEXT("");
    }

    base = path;
    for (cursor = path; *cursor != 0; ++cursor) {
        if (*cursor == TEXT('\\') || *cursor == TEXT('/')) {
            base = cursor + 1;
        }
    }

    return base;
}

static void App_AppendText(WCHAR* buffer, size_t buffer_count, const WCHAR* text) {
    size_t length;

    if (!buffer || buffer_count == 0 || !text) {
        return;
    }

    length = (size_t)lstrlen(buffer);
    if (length >= buffer_count - 1) {
        return;
    }

    lstrcpyn(buffer + length, text, (int)(buffer_count - length));
}

static void App_AppendUnsigned(WCHAR* buffer, size_t buffer_count, unsigned int value) {
    WCHAR number[16];

    wsprintf(number, TEXT("%u"), value);
    App_AppendText(buffer, buffer_count, number);
}

static void App_GetDocumentLabel(const DOCUMENT* doc, WCHAR* buffer, size_t buffer_count) {
    const WCHAR* base_name;

    if (!doc || !buffer || buffer_count == 0) {
        return;
    }

    buffer[0] = 0;
    if (doc->path[0] != 0) {
        base_name = App_GetBaseName(doc->path);
        if (doc->dirty) {
            App_AppendText(buffer, buffer_count, TEXT("*"));
        }
        App_AppendText(buffer, buffer_count, base_name);
    } else {
        if (doc->dirty) {
            App_AppendText(buffer, buffer_count, TEXT("*"));
        }
        App_AppendText(buffer, buffer_count, TEXT("Untitled "));
        App_AppendUnsigned(buffer, buffer_count, doc->untitled_id);
    }
}

static void App_UpdateCaption(APP_STATE* app) {
    WCHAR label[96];
    WCHAR caption[160];

    if (!app || app->doc_count == 0) {
        return;
    }

    App_GetDocumentLabel(&app->docs[app->active_doc], label, sizeof(label) / sizeof(label[0]));
    caption[0] = 0;
    App_AppendText(caption, sizeof(caption) / sizeof(caption[0]), APP_TITLE);
    App_AppendText(caption, sizeof(caption) / sizeof(caption[0]), TEXT(" - "));
    App_AppendText(caption, sizeof(caption) / sizeof(caption[0]), label);
    SetWindowText(app->window, caption);
}

static void App_UpdateTabText(APP_STATE* app, int index) {
    TCITEM item;
    WCHAR label[96];

    if (!app || index < 0 || (size_t)index >= app->doc_count) {
        return;
    }

    memset(&item, 0, sizeof(item));
    item.mask = TCIF_TEXT;
    App_GetDocumentLabel(&app->docs[index], label, sizeof(label) / sizeof(label[0]));
    item.pszText = label;
    TabCtrl_SetItem(app->tab, index, &item);

    if (index == app->active_doc) {
        App_UpdateCaption(app);
    }
}

static void App_UpdateAllTabText(APP_STATE* app) {
    int index;

    if (!app) {
        return;
    }

    for (index = 0; (size_t)index < app->doc_count; ++index) {
        App_UpdateTabText(app, index);
    }
}

static HMENU App_GetTabsMenu(APP_STATE* app) {
    if (!app || !app->menu) {
        return NULL;
    }

    return GetSubMenu(app->menu, 3);
}

static void App_RebuildTabsMenu(APP_STATE* app) {
    HMENU tabs_menu;
    int index;

    tabs_menu = App_GetTabsMenu(app);
    if (!tabs_menu) {
        return;
    }

    /* DeleteMenu returns nonzero while items remain; loop clears all dynamic tab entries. */
    while (DeleteMenu(tabs_menu, 2, MF_BYPOSITION)) {
    }

    if (app->doc_count == 0) {
        return;
    }

    AppendMenu(tabs_menu, MF_SEPARATOR, 0, NULL);
    for (index = 0; (size_t)index < app->doc_count; ++index) {
        WCHAR document_label[96];
        WCHAR menu_label[128];

        App_GetDocumentLabel(&app->docs[index], document_label, sizeof(document_label) / sizeof(document_label[0]));
        menu_label[0] = 0;
        App_AppendUnsigned(menu_label, sizeof(menu_label) / sizeof(menu_label[0]), (unsigned int)(index + 1));
        App_AppendText(menu_label, sizeof(menu_label) / sizeof(menu_label[0]), TEXT(" "));
        App_AppendText(menu_label, sizeof(menu_label) / sizeof(menu_label[0]), document_label);
        AppendMenu(tabs_menu, MF_STRING, ID_TABS_DOCUMENT_FIRST + index, menu_label);
        if (index == app->active_doc) {
            CheckMenuItem(tabs_menu, ID_TABS_DOCUMENT_FIRST + index, MF_BYCOMMAND | MF_CHECKED);
        }
    }
}

static DWORD App_GetEditStyle(const APP_STATE* app) {
    DWORD style;

    style = WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_NOHIDESEL | ES_WANTRETURN;
    if (!app->word_wrap) {
        style |= WS_HSCROLL | ES_AUTOHSCROLL;
    }

    return style;
}

static LRESULT CALLBACK App_EditProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == WM_GETDLGCODE) {
        return CallWindowProc(g_edit_wndproc, window, message, w_param, l_param) | DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    }

    return CallWindowProc(g_edit_wndproc, window, message, w_param, l_param);
}

static LRESULT CALLBACK App_TabProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    if (message == WM_COMMAND) {
        HWND parent_window;

        parent_window = GetParent(window);
        if (parent_window) {
            SendMessage(parent_window, WM_COMMAND, w_param, l_param);
            return 0;
        }
    }

    return CallWindowProc(g_tab_wndproc, window, message, w_param, l_param);
}

static HWND App_CreateEditControl(APP_STATE* app, LPCTSTR text) {
    HWND edit;

    edit = CreateWindowEx(
        WS_EX_CLIENTEDGE,
        TEXT("EDIT"),
        text ? text : TEXT(""),
        App_GetEditStyle(app),
        0,
        0,
        0,
        0,
        app->tab,
        NULL,
        app->instance,
        NULL
    );

    if (!edit) {
        return NULL;
    }

    if (!g_edit_wndproc) {
        g_edit_wndproc = (WNDPROC)GetWindowLongPtr(edit, GWLP_WNDPROC);
    }
    SetWindowLongPtr(edit, GWLP_WNDPROC, (LONG_PTR)App_EditProc);
    SendMessage(edit, WM_SETFONT, (WPARAM)app->edit_font, MAKELPARAM(TRUE, 0));
    SendMessage(edit, EM_LIMITTEXT, 0, 0);
    return edit;
}

static void App_Layout(APP_STATE* app) {
    RECT client;
    RECT tab_rect;
    RECT editor_rect;
    int index;
    int width;
    int height;
    int margin;
    int tab_left;
    int tab_top;
    int tab_width;
    int tab_height;
    int command_height;

    if (!app || !app->tab) {
        return;
    }

    GetClientRect(app->window, &client);
    width = client.right - client.left;
    height = client.bottom - client.top;
    margin = 4;

    if (app->command_bar) {
        command_height = CommandBar_Height(app->command_bar);
        MoveWindow(app->command_bar, 0, 0, width, command_height, TRUE);
        client.top += command_height;
        height = client.bottom - client.top;
    }

    tab_left = margin;
    tab_top = client.top + margin;
    tab_width = width - (margin * 2);
    tab_height = height - (margin * 2);

    MoveWindow(app->tab, tab_left, tab_top, tab_width, tab_height, TRUE);

    SetRect(&tab_rect, 0, 0, tab_width, tab_height);
    editor_rect = tab_rect;
    TabCtrl_AdjustRect(app->tab, FALSE, &editor_rect);

    for (index = 0; (size_t)index < app->doc_count; ++index) {
        MoveWindow(
            app->docs[index].edit,
            editor_rect.left,
            editor_rect.top,
            editor_rect.right - editor_rect.left,
            editor_rect.bottom - editor_rect.top,
            TRUE
        );
    }
}

static void App_ShowActiveDocument(APP_STATE* app) {
    int index;

    if (!app || app->doc_count == 0) {
        return;
    }

    for (index = 0; (size_t)index < app->doc_count; ++index) {
        ShowWindow(app->docs[index].edit, index == app->active_doc ? SW_SHOW : SW_HIDE);
    }

    App_UpdateCaption(app);
    SetFocus(app->docs[app->active_doc].edit);
}

static int App_EnsureDocumentCapacity(APP_STATE* app, size_t needed) {
    DOCUMENT* grown;
    size_t capacity;

    if (needed <= app->doc_capacity) {
        return 1;
    }

    capacity = app->doc_capacity == 0 ? 4 : app->doc_capacity;
    while (capacity < needed) {
        capacity *= 2;
    }

    grown = (DOCUMENT*)realloc(app->docs, capacity * sizeof(DOCUMENT));
    if (!grown) {
        return 0;
    }

    app->docs = grown;
    app->doc_capacity = capacity;
    return 1;
}

static HWND App_GetActiveEdit(APP_STATE* app) {
    if (!app || app->doc_count == 0) {
        return NULL;
    }

    return app->docs[app->active_doc].edit;
}

static int App_IsEditorKeyTarget(APP_STATE* app, HWND target) {
    HWND active_edit;

    if (!app || !target) {
        return 0;
    }

    if (app->find_window && (target == app->find_window || IsChild(app->find_window, target))) {
        return 0;
    }

    if (app->command_bar && (target == app->command_bar || IsChild(app->command_bar, target))) {
        return 0;
    }

    active_edit = App_GetActiveEdit(app);
    if (!active_edit) {
        return 0;
    }

    if (target == active_edit || IsChild(active_edit, target)) {
        return 1;
    }

    if (target == app->tab || IsChild(app->tab, target)) {
        return 1;
    }

    return 0;
}

static void App_SelectDocument(APP_STATE* app, int index) {
    if (!app || index < 0 || (size_t)index >= app->doc_count) {
        return;
    }

    app->active_doc = index;
    TabCtrl_SetCurSel(app->tab, index);
    App_ShowActiveDocument(app);
}

static void App_SetDocumentDirty(APP_STATE* app, int index, int dirty) {
    if (!app || index < 0 || (size_t)index >= app->doc_count) {
        return;
    }

    if (app->docs[index].dirty == dirty) {
        return;
    }

    app->docs[index].dirty = dirty;
    App_UpdateTabText(app, index);
}

static WCHAR* App_GetEditTextCopy(HWND edit) {
    int length;
    WCHAR* buffer;

    length = GetWindowTextLength(edit);
    buffer = (WCHAR*)malloc(((size_t)length + 1) * sizeof(WCHAR));
    if (!buffer) {
        return NULL;
    }

    GetWindowText(edit, buffer, length + 1);
    return buffer;
}

static int App_InsertDocument(APP_STATE* app, LPCTSTR text, const WCHAR* path) {
    DOCUMENT* doc;
    TCITEM tab_item;
    WCHAR label[96];
    HWND edit;

    if (!App_EnsureDocumentCapacity(app, app->doc_count + 1)) {
        MessageBox(app->window, TEXT("Unable to allocate a new tab."), APP_TITLE, MB_OK | MB_ICONERROR);
        return -1;
    }

    edit = App_CreateEditControl(app, text);
    if (!edit) {
        MessageBox(app->window, TEXT("The editor control could not be created."), APP_TITLE, MB_OK | MB_ICONERROR);
        return -1;
    }

    doc = &app->docs[app->doc_count];
    memset(doc, 0, sizeof(*doc));
    doc->edit = edit;
    doc->untitled_id = app->next_untitled_id++;
    if (path && path[0] != 0) {
        lstrcpyn(doc->path, path, MAX_PATH);
    }

    App_GetDocumentLabel(doc, label, sizeof(label) / sizeof(label[0]));
    memset(&tab_item, 0, sizeof(tab_item));
    tab_item.mask = TCIF_TEXT;
    tab_item.pszText = label;
    TabCtrl_InsertItem(app->tab, (int)app->doc_count, &tab_item);

    app->doc_count++;
    app->active_doc = (int)(app->doc_count - 1);
    App_Layout(app);
    App_ShowActiveDocument(app);
    return app->active_doc;
}

static int App_LoadFileText(const WCHAR* path, WCHAR** text_out) {
    HANDLE file;
    DWORD size;
    DWORD read_size;
    BYTE* bytes;
    BYTE* data;
    WCHAR* text;
    int char_count;

    *text_out = NULL;

    file = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }

    size = GetFileSize(file, NULL);
    if (size == INVALID_FILE_SIZE || size > APP_MAX_FILE_SIZE) {
        CloseHandle(file);
        return 0;
    }

    bytes = (BYTE*)malloc((size_t)size + 2);
    if (!bytes) {
        CloseHandle(file);
        return 0;
    }

    read_size = 0;
    if (size != 0 && !ReadFile(file, bytes, size, &read_size, NULL)) {
        free(bytes);
        CloseHandle(file);
        return 0;
    }
    CloseHandle(file);

    data = bytes;

    if (read_size >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
        size_t wide_bytes;

        wide_bytes = read_size - 2;
        text = (WCHAR*)malloc(wide_bytes + sizeof(WCHAR));
        if (!text) {
            free(bytes);
            return 0;
        }

        memcpy(text, data + 2, wide_bytes);
        text[wide_bytes / sizeof(WCHAR)] = 0;
        *text_out = text;
        free(bytes);
        return 1;
    }

    if (read_size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        data += 3;
        read_size -= 3;
    }

    char_count = MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)data, (int)read_size, NULL, 0);
    if (char_count <= 0) {
        char_count = MultiByteToWideChar(CP_ACP, 0, (LPCSTR)data, (int)read_size, NULL, 0);
        if (char_count <= 0) {
            if (read_size == 0) {
                text = (WCHAR*)malloc(sizeof(WCHAR));
                if (!text) {
                    free(bytes);
                    return 0;
                }

                text[0] = 0;
                *text_out = text;
                free(bytes);
                return 1;
            }

            free(bytes);
            return 0;
        }

        if ((size_t)char_count > (SIZE_MAX / sizeof(WCHAR)) - 1) {
            free(bytes);
            return 0;
        }
        text = (WCHAR*)malloc(((size_t)char_count + 1) * sizeof(WCHAR));
        if (!text) {
            free(bytes);
            return 0;
        }

        if (MultiByteToWideChar(CP_ACP, 0, (LPCSTR)data, (int)read_size, text, char_count) <= 0) {
            free(text);
            free(bytes);
            return 0;
        }
    } else {
        if ((size_t)char_count > (SIZE_MAX / sizeof(WCHAR)) - 1) {
            free(bytes);
            return 0;
        }
        text = (WCHAR*)malloc(((size_t)char_count + 1) * sizeof(WCHAR));
        if (!text) {
            free(bytes);
            return 0;
        }

        if (MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)data, (int)read_size, text, char_count) <= 0) {
            free(text);
            free(bytes);
            return 0;
        }
    }

    text[char_count] = 0;
    *text_out = text;
    free(bytes);
    return 1;
}

static int App_SaveFileText(const WCHAR* path, const WCHAR* text) {
    HANDLE file;
    BYTE bom[3];
    char* bytes;
    DWORD written;
    int byte_count;

    file = CreateFile(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }

    bom[0] = 0xEF;
    bom[1] = 0xBB;
    bom[2] = 0xBF;
    if (!WriteFile(file, bom, sizeof(bom), &written, NULL) || written != sizeof(bom)) {
        CloseHandle(file);
        return 0;
    }

    byte_count = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    if (byte_count <= 0) {
        CloseHandle(file);
        return 0;
    }

    bytes = (char*)malloc((size_t)byte_count);
    if (!bytes) {
        CloseHandle(file);
        return 0;
    }

    WideCharToMultiByte(CP_UTF8, 0, text, -1, bytes, byte_count, NULL, NULL);
    if (!WriteFile(file, bytes, (DWORD)(byte_count - 1), &written, NULL) || written != (DWORD)(byte_count - 1)) {
        free(bytes);
        CloseHandle(file);
        return 0;
    }

    free(bytes);
    CloseHandle(file);
    return 1;
}

static int App_BrowseForPath(HWND owner, WCHAR* path, DWORD flags, LPCTSTR title) {
    OPENFILENAME info;
    static const TCHAR filter[] = TEXT("Text Files (*.txt)\0*.txt\0All Files (*.*)\0*.*\0\0");

    memset(&info, 0, sizeof(info));
    info.lStructSize = sizeof(info);
    info.hwndOwner = owner;
    info.lpstrFilter = filter;
    info.lpstrFile = path;
    info.nMaxFile = MAX_PATH;
    info.lpstrTitle = title;
    info.Flags = flags;
    info.lpstrDefExt = TEXT("txt");

    if (flags & OFN_OVERWRITEPROMPT) {
        if (GetSaveFileName(&info)) {
            return 1;
        }
    } else {
        if (GetOpenFileName(&info)) {
            return 1;
        }
    }

    if (CommDlgExtendedError() != 0) {
        MessageBox(owner, TEXT("The file dialog encountered an error."), APP_TITLE, MB_OK | MB_ICONERROR);
    }
    return 0;
}

static int App_OpenDocumentFromPath(APP_STATE* app, const WCHAR* path) {
    WCHAR* text;
    int index;

    text = NULL;
    if (!App_LoadFileText(path, &text)) {
        MessageBox(app->window, TEXT("The file could not be opened."), APP_TITLE, MB_OK | MB_ICONERROR);
        return 0;
    }

    index = App_InsertDocument(app, text, path);
    free(text);
    if (index < 0) {
        return 0;
    }

    SendMessage(app->docs[index].edit, EM_SETMODIFY, FALSE, 0);
    App_SetDocumentDirty(app, index, 0);
    App_SelectDocument(app, index);
    return 1;
}

static int App_SaveDocument(APP_STATE* app, int index, int save_as) {
    WCHAR path[MAX_PATH];
    WCHAR* text;

    if (!app || index < 0 || (size_t)index >= app->doc_count) {
        return 0;
    }

    if (save_as || app->docs[index].path[0] == 0) {
        lstrcpyn(path, app->docs[index].path, MAX_PATH);
        if (!App_BrowseForPath(app->window, path, OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT, TEXT("Save Text File"))) {
            return 0;
        }
        lstrcpyn(app->docs[index].path, path, MAX_PATH);
    }

    text = App_GetEditTextCopy(app->docs[index].edit);
    if (!text) {
        MessageBox(app->window, TEXT("The document could not be read from the editor."), APP_TITLE, MB_OK | MB_ICONERROR);
        return 0;
    }

    if (!App_SaveFileText(app->docs[index].path, text)) {
        free(text);
        MessageBox(app->window, TEXT("The file could not be saved."), APP_TITLE, MB_OK | MB_ICONERROR);
        return 0;
    }

    free(text);
    SendMessage(app->docs[index].edit, EM_SETMODIFY, FALSE, 0);
    App_SetDocumentDirty(app, index, 0);
    return 1;
}

static int App_PromptSaveIfNeeded(APP_STATE* app, int index) {
    WCHAR label[96];
    WCHAR prompt[160];
    int result;

    if (!app || index < 0 || (size_t)index >= app->doc_count || !app->docs[index].dirty) {
        return 1;
    }

    App_GetDocumentLabel(&app->docs[index], label, sizeof(label) / sizeof(label[0]));
    prompt[0] = 0;
    App_AppendText(prompt, sizeof(prompt) / sizeof(prompt[0]), TEXT("Save changes to "));
    App_AppendText(prompt, sizeof(prompt) / sizeof(prompt[0]), label);
    App_AppendText(prompt, sizeof(prompt) / sizeof(prompt[0]), TEXT("?"));
    result = MessageBox(app->window, prompt, APP_TITLE, MB_YESNOCANCEL | MB_ICONQUESTION);
    if (result == IDCANCEL) {
        return 0;
    }

    if (result == IDYES) {
        return App_SaveDocument(app, index, 0);
    }

    return 1;
}

static int App_PromptActiveDocumentIfNeeded(APP_STATE* app) {
    if (!app || app->active_doc < 0 || (size_t)app->active_doc >= app->doc_count) {
        return 1;
    }

    return App_PromptSaveIfNeeded(app, app->active_doc);
}

static void App_ResetDocumentToBlank(APP_STATE* app, int index) {
    if (!app || index < 0 || (size_t)index >= app->doc_count) {
        return;
    }

    app->docs[index].path[0] = 0;
    app->docs[index].dirty = 0;
    app->docs[index].untitled_id = app->next_untitled_id++;
    SetWindowText(app->docs[index].edit, TEXT(""));
    SendMessage(app->docs[index].edit, EM_SETMODIFY, FALSE, 0);
    App_UpdateTabText(app, index);
}

static int App_CloseDocument(APP_STATE* app, int index) {
    size_t move_count;

    if (!app || index < 0 || (size_t)index >= app->doc_count) {
        return 0;
    }

    if (!App_PromptSaveIfNeeded(app, index)) {
        return 0;
    }

    if (app->doc_count == 1) {
        App_ResetDocumentToBlank(app, index);
        App_SelectDocument(app, 0);
        return 1;
    }

    DestroyWindow(app->docs[index].edit);
    TabCtrl_DeleteItem(app->tab, index);

    move_count = app->doc_count - (size_t)index - 1;
    if (move_count > 0) {
        memmove(&app->docs[index], &app->docs[index + 1], move_count * sizeof(DOCUMENT));
    }
    app->doc_count--;

    if (app->active_doc >= (int)app->doc_count) {
        app->active_doc = (int)app->doc_count - 1;
    } else if (index <= app->active_doc && app->active_doc > 0) {
        app->active_doc--;
    }

    TabCtrl_SetCurSel(app->tab, app->active_doc);
    App_Layout(app);
    App_ShowActiveDocument(app);
    App_UpdateAllTabText(app);
    return 1;
}

static int App_CloseAllDocuments(APP_STATE* app) {
    int index;

    if (!app) {
        return 0;
    }

    for (index = (int)app->doc_count - 1; index >= 0; --index) {
        App_SelectDocument(app, index);
        if (!App_PromptSaveIfNeeded(app, index)) {
            return 0;
        }
    }

    return 1;
}

static void App_UpdateMenuState(APP_STATE* app) {
    HWND edit;
    DWORD start;
    DWORD end;

    if (!app || !app->menu) {
        return;
    }

    edit = App_GetActiveEdit(app);
    start = 0;
    end = 0;
    if (edit) {
        SendMessage(edit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    }

    CheckMenuItem(app->menu, ID_EDIT_WORD_WRAP, MF_BYCOMMAND | (app->word_wrap ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(app->menu, ID_EDIT_UNDO, MF_BYCOMMAND | (edit && SendMessage(edit, EM_CANUNDO, 0, 0) ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(app->menu, ID_EDIT_CUT, MF_BYCOMMAND | (edit && end > start ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(app->menu, ID_EDIT_COPY, MF_BYCOMMAND | (edit && end > start ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(app->menu, ID_EDIT_DELETE, MF_BYCOMMAND | (edit && end > start ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(
        app->menu,
        ID_EDIT_PASTE,
        MF_BYCOMMAND | (edit && (IsClipboardFormatAvailable(CF_UNICODETEXT) || IsClipboardFormatAvailable(CF_TEXT)) ? MF_ENABLED : MF_GRAYED)
    );
    EnableMenuItem(app->menu, ID_TABS_NEXT, MF_BYCOMMAND | (app->doc_count > 1 ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(app->menu, ID_TABS_PREVIOUS, MF_BYCOMMAND | (app->doc_count > 1 ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(app->menu, ID_SEARCH_FIND_NEXT, MF_BYCOMMAND | (app->find_buffer[0] != 0 ? MF_ENABLED : MF_GRAYED));
}

static void App_RecreateEditors(APP_STATE* app) {
    int index;

    for (index = 0; (size_t)index < app->doc_count; ++index) {
        DWORD start;
        DWORD end;
        WCHAR* text;
        HWND edit;

        text = App_GetEditTextCopy(app->docs[index].edit);
        if (!text) {
            continue;
        }

        start = 0;
        end = 0;
        SendMessage(app->docs[index].edit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
        DestroyWindow(app->docs[index].edit);

        edit = App_CreateEditControl(app, text);
        free(text);
        if (!edit) {
            continue;
        }

        app->docs[index].edit = edit;
        SendMessage(edit, EM_SETSEL, start, end);
        SendMessage(edit, EM_SETMODIFY, app->docs[index].dirty ? TRUE : FALSE, 0);
    }

    App_Layout(app);
    App_ShowActiveDocument(app);
}

static int App_TextMatchesAt(const WCHAR* haystack, int offset, const WCHAR* needle, int case_sensitive) {
    int needle_length;
    DWORD flags;

    needle_length = lstrlen(needle);
    flags = case_sensitive ? 0 : NORM_IGNORECASE;

    return CompareString(LOCALE_USER_DEFAULT, flags, haystack + offset, needle_length, needle, needle_length) == CSTR_EQUAL;
}

static int App_FindInActiveDocument(APP_STATE* app, int reverse, int case_sensitive) {
    HWND edit;
    WCHAR* text;
    int text_length;
    int key_length;
    DWORD start;
    DWORD end;
    int index;

    if (!app || app->find_buffer[0] == 0) {
        return 0;
    }

    edit = App_GetActiveEdit(app);
    if (!edit) {
        return 0;
    }

    text = App_GetEditTextCopy(edit);
    if (!text) {
        return 0;
    }

    text_length = lstrlen(text);
    key_length = lstrlen(app->find_buffer);
    if (key_length == 0 || key_length > text_length) {
        free(text);
        return 0;
    }

    start = 0;
    end = 0;
    SendMessage(edit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);

    if (!reverse) {
        for (index = (int)end; index <= text_length - key_length; ++index) {
            if (App_TextMatchesAt(text, index, app->find_buffer, case_sensitive)) {
                SendMessage(edit, EM_SETSEL, index, index + key_length);
                SendMessage(edit, EM_SCROLLCARET, 0, 0);
                SetFocus(edit);
                free(text);
                return 1;
            }
        }
    } else {
        int start_index;

        start_index = (int)start - key_length;
        if (start_index > text_length - key_length) {
            start_index = text_length - key_length;
        }
        for (index = start_index; index >= 0; --index) {
            if (App_TextMatchesAt(text, index, app->find_buffer, case_sensitive)) {
                SendMessage(edit, EM_SETSEL, index, index + key_length);
                SendMessage(edit, EM_SCROLLCARET, 0, 0);
                SetFocus(edit);
                free(text);
                return 1;
            }
        }
    }

    free(text);
    MessageBox(app->window, TEXT("Cannot find the requested text."), APP_TITLE, MB_OK | MB_ICONINFORMATION);
    return 0;
}

static void App_SyncFindSettingsFromWindow(APP_STATE* app) {
    if (!app || !app->find_window) {
        return;
    }

    GetWindowText(app->find_edit, app->find_buffer, FIND_BUFFER_LENGTH);
    app->find_match_case = SendMessage(app->find_match_case_box, BM_GETCHECK, 0, 0) == BST_CHECKED;
    app->find_reverse = SendMessage(app->find_up_radio, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void App_InsertDateTime(APP_STATE* app) {
    SYSTEMTIME now;
    WCHAR date_buffer[80];
    WCHAR time_buffer[80];
    HWND edit;

    edit = App_GetActiveEdit(app);
    if (!edit) {
        return;
    }

    GetLocalTime(&now);
    GetTimeFormat(LOCALE_USER_DEFAULT, 0, &now, NULL, time_buffer, sizeof(time_buffer) / sizeof(time_buffer[0]));
    GetDateFormat(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &now, NULL, date_buffer, sizeof(date_buffer) / sizeof(date_buffer[0]));

    SendMessage(edit, EM_REPLACESEL, FALSE, (LPARAM)time_buffer);
    SendMessage(edit, EM_REPLACESEL, FALSE, (LPARAM)TEXT(" "));
    SendMessage(edit, EM_REPLACESEL, FALSE, (LPARAM)date_buffer);
}

static void App_HandleClipboardCommand(APP_STATE* app, UINT command_id) {
    HWND edit;

    edit = App_GetActiveEdit(app);
    if (!edit) {
        return;
    }

    switch (command_id) {
        case ID_EDIT_UNDO:
            SendMessage(edit, WM_UNDO, 0, 0);
            break;
        case ID_EDIT_CUT:
            SendMessage(edit, WM_CUT, 0, 0);
            break;
        case ID_EDIT_COPY:
            SendMessage(edit, WM_COPY, 0, 0);
            break;
        case ID_EDIT_PASTE:
            SendMessage(edit, WM_PASTE, 0, 0);
            break;
        case ID_EDIT_DELETE:
            SendMessage(edit, WM_CLEAR, 0, 0);
            break;
        case ID_EDIT_SELECT_ALL:
            SendMessage(edit, EM_SETSEL, 0, -1);
            break;
    }

    SetFocus(edit);
}

static void App_SwitchDocument(APP_STATE* app, int direction) {
    int index;

    if (!app || app->doc_count == 0) {
        return;
    }

    index = app->active_doc + direction;
    if (index < 0) {
        index = (int)app->doc_count - 1;
    } else if ((size_t)index >= app->doc_count) {
        index = 0;
    }

    App_SelectDocument(app, index);
}

static void App_SetEditFont(APP_STATE* app, HFONT font, const LOGFONT* logfont, int is_custom) {
    int index;

    if (!app || !font) {
        return;
    }

    if (app->edit_font && app->owns_edit_font) {
        DeleteObject(app->edit_font);
    }

    app->edit_font = font;
    app->has_custom_font = is_custom;
    app->owns_edit_font = 1;
    if (logfont) {
        memcpy(&app->edit_logfont, logfont, sizeof(app->edit_logfont));
    } else {
        memset(&app->edit_logfont, 0, sizeof(app->edit_logfont));
    }

    for (index = 0; (size_t)index < app->doc_count; ++index) {
        SendMessage(app->docs[index].edit, WM_SETFONT, (WPARAM)app->edit_font, MAKELPARAM(TRUE, 0));
    }

    InvalidateRect(app->window, NULL, TRUE);
}

static void App_ChooseFont(APP_STATE* app) {
    CHOOSEFONT choose_font;
    LOGFONT logfont;
    HFONT font;

    if (!app) {
        return;
    }

    memcpy(&logfont, &app->edit_logfont, sizeof(logfont));

    memset(&choose_font, 0, sizeof(choose_font));
    choose_font.lStructSize = sizeof(choose_font);
    choose_font.hwndOwner = app->window;
    choose_font.lpLogFont = &logfont;
    choose_font.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT;

    if (!ChooseFont(&choose_font)) {
        return;
    }

    font = CreateFontIndirect(&logfont);
    if (!font) {
        MessageBox(app->window, TEXT("The selected font could not be created."), APP_TITLE, MB_OK | MB_ICONERROR);
        return;
    }

    App_SetEditFont(app, font, &logfont, 1);
}

static HACCEL App_CreateAccelerators(void) {
    ACCEL entries[] = {
        { FVIRTKEY | FCONTROL, 'N', ID_FILE_NEW },
        { FVIRTKEY | FCONTROL, 'O', ID_FILE_OPEN },
        { FVIRTKEY | FCONTROL, 'S', ID_FILE_SAVE },
        { FVIRTKEY | FCONTROL | FSHIFT, 'S', ID_FILE_SAVE_AS },
        { FVIRTKEY | FCONTROL, 'W', ID_FILE_CLOSE_TAB },
        { FVIRTKEY | FCONTROL, 'Z', ID_EDIT_UNDO },
        { FVIRTKEY | FCONTROL, 'X', ID_EDIT_CUT },
        { FVIRTKEY | FCONTROL, 'C', ID_EDIT_COPY },
        { FVIRTKEY | FCONTROL, 'V', ID_EDIT_PASTE },
        { FVIRTKEY | FCONTROL, 'A', ID_EDIT_SELECT_ALL },
        { FVIRTKEY | FCONTROL, 'F', ID_SEARCH_FIND },
        { FVIRTKEY | FALT, VK_F4, ID_FILE_EXIT },
        { FVIRTKEY, VK_DELETE, ID_EDIT_DELETE },
        { FVIRTKEY, VK_F3, ID_SEARCH_FIND_NEXT },
        { FVIRTKEY, VK_F5, ID_EDIT_TIME_DATE },
        { FVIRTKEY | FCONTROL, VK_TAB, ID_TABS_NEXT },
        { FVIRTKEY | FCONTROL | FSHIFT, VK_TAB, ID_TABS_PREVIOUS }
    };

    return CreateAcceleratorTable(entries, sizeof(entries) / sizeof(entries[0]));
}

static void App_CreateControls(APP_STATE* app) {
    app->menu = LoadMenu(app->instance, MAKEINTRESOURCE(IDR_MAIN_MENU));
    app->command_bar = CommandBar_Create(app->instance, app->window, 1);
    if (app->command_bar) {
        CommandBar_InsertMenubar(app->command_bar, app->instance, IDR_MAIN_MENU, 0);
        CommandBar_DrawMenuBar(app->command_bar, 0);
        app->menu = CommandBar_GetMenu(app->command_bar, 0);
    }

    app->tab = CreateWindow(
        WC_TABCONTROL,
        TEXT(""),
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
        0,
        0,
        0,
        0,
        app->window,
        (HMENU)IDC_TAB,
        app->instance,
        NULL
    );

    if (!g_tab_wndproc) {
        g_tab_wndproc = (WNDPROC)GetWindowLongPtr(app->tab, GWLP_WNDPROC);
    }
    SetWindowLongPtr(app->tab, GWLP_WNDPROC, (LONG_PTR)App_TabProc);
    SendMessage(app->tab, WM_SETFONT, (WPARAM)app->ui_font, MAKELPARAM(TRUE, 0));
}

static void App_HandleOpen(APP_STATE* app) {
    WCHAR path[MAX_PATH];

    path[0] = 0;
    if (App_BrowseForPath(app->window, path, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY, TEXT("Open Text File")) &&
        App_PromptActiveDocumentIfNeeded(app)) {
        App_OpenDocumentFromPath(app, path);
    }
}

static void App_ShowAbout(APP_STATE* app) {
    MessageBox(app->window, TEXT("CE Pad\n\nTabbed Notepad-style editor for Windows CE."), APP_TITLE, MB_OK | MB_ICONINFORMATION);
}

static LRESULT CALLBACK App_FindWindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    APP_STATE* app;

    app = (APP_STATE*)GetWindowLongPtr(window, GWLP_USERDATA);

    switch (message) {
        case WM_CREATE:
        {
            CREATESTRUCT* create_info;
            HFONT font;

            create_info = (CREATESTRUCT*)l_param;
            app = (APP_STATE*)create_info->lpCreateParams;
            SetWindowLongPtr(window, GWLP_USERDATA, (LONG_PTR)app);
            app->find_window = window;
            font = app->ui_font;

            CreateWindow(TEXT("STATIC"), TEXT("Find what:"), WS_CHILD | WS_VISIBLE, 8, 10, 52, 14, window, NULL, app->instance, NULL);
            app->find_edit = CreateWindowEx(WS_EX_CLIENTEDGE, TEXT("EDIT"), app->find_buffer, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 64, 8, 110, 18, window, (HMENU)IDC_FIND_TEXT, app->instance, NULL);
            app->find_match_case_box = CreateWindow(TEXT("BUTTON"), TEXT("Match case"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 8, 34, 68, 14, window, (HMENU)IDC_FIND_MATCH_CASE, app->instance, NULL);
            app->find_up_radio = CreateWindow(TEXT("BUTTON"), TEXT("Up"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, 86, 34, 28, 14, window, (HMENU)IDC_FIND_DIRECTION_UP, app->instance, NULL);
            app->find_down_radio = CreateWindow(TEXT("BUTTON"), TEXT("Down"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, 122, 34, 40, 14, window, (HMENU)IDC_FIND_DIRECTION_DOWN, app->instance, NULL);
            CreateWindow(TEXT("BUTTON"), TEXT("Find Next"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 64, 58, 52, 18, window, (HMENU)IDOK, app->instance, NULL);
            CreateWindow(TEXT("BUTTON"), TEXT("Cancel"), WS_CHILD | WS_VISIBLE | WS_TABSTOP, 122, 58, 52, 18, window, (HMENU)IDCANCEL, app->instance, NULL);

            SendMessage(app->find_edit, WM_SETFONT, (WPARAM)font, TRUE);
            SendMessage(app->find_match_case_box, WM_SETFONT, (WPARAM)font, TRUE);
            SendMessage(app->find_up_radio, WM_SETFONT, (WPARAM)font, TRUE);
            SendMessage(app->find_down_radio, WM_SETFONT, (WPARAM)font, TRUE);
            SendDlgItemMessage(window, IDOK, WM_SETFONT, (WPARAM)font, TRUE);
            SendDlgItemMessage(window, IDCANCEL, WM_SETFONT, (WPARAM)font, TRUE);

            SendMessage(app->find_match_case_box, BM_SETCHECK, app->find_match_case ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessage(app->find_reverse ? app->find_up_radio : app->find_down_radio, BM_SETCHECK, BST_CHECKED, 0);
            SetFocus(app->find_edit);
            return 0;
        }

        case WM_SETFOCUS:
            if (app && app->find_edit) {
                SetFocus(app->find_edit);
            }
            return 0;

        case WM_COMMAND:
            if (!app) {
                return 0;
            }

            switch (LOWORD(w_param)) {
                case IDOK:
                    App_SyncFindSettingsFromWindow(app);
                    App_FindInActiveDocument(app, app->find_reverse, app->find_match_case);
                    App_UpdateMenuState(app);
                    return 0;

                case IDCANCEL:
                    DestroyWindow(window);
                    return 0;
            }
            return 0;

        case WM_CLOSE:
            DestroyWindow(window);
            return 0;

        case WM_DESTROY:
            if (app) {
                app->find_window = NULL;
                app->find_edit = NULL;
                app->find_match_case_box = NULL;
                app->find_up_radio = NULL;
                app->find_down_radio = NULL;
            }
            return 0;
    }

    return DefWindowProc(window, message, w_param, l_param);
}

static void App_ShowFindDialog(APP_STATE* app) {
    if (!app) {
        return;
    }

    if (app->find_window) {
        ShowWindow(app->find_window, SW_SHOW);
        SetForegroundWindow(app->find_window);
        SetFocus(app->find_edit);
        return;
    }

    CreateWindowEx(
        WS_EX_DLGMODALFRAME,
        FIND_CLASS_NAME,
        TEXT("Find"),
        WS_CAPTION | WS_SYSMENU | WS_POPUPWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        188,
        110,
        app->window,
        NULL,
        app->instance,
        app
    );
}

static LRESULT CALLBACK App_WindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    APP_STATE* app;

    app = App_GetState(window);

    switch (message) {
        case WM_CREATE:
        {
            CREATESTRUCT* create_info;
            HFONT default_font;

            create_info = (CREATESTRUCT*)l_param;
            app = (APP_STATE*)create_info->lpCreateParams;
            app->window = window;
            SetWindowLongPtr(window, GWLP_USERDATA, (LONG_PTR)app);
            default_font = App_CreateDefaultEditFont(&app->edit_logfont);
            if (default_font) {
                app->owns_edit_font = 1;
            } else {
                default_font = (HFONT)GetStockObject(SYSTEM_FIXED_FONT);
                app->owns_edit_font = 0;
                GetObject(default_font, sizeof(app->edit_logfont), &app->edit_logfont);
            }
            app->edit_font = default_font;

            App_CreateControls(app);
            App_InsertDocument(app, TEXT(""), NULL);
            App_UpdateMenuState(app);
            App_Layout(app);
            return 0;
        }

        case WM_SIZE:
            if (app) {
                App_Layout(app);
            }
            return 0;

        case WM_SETFOCUS:
            if (app && app->doc_count > 0) {
                SetFocus(app->docs[app->active_doc].edit);
            }
            return 0;

        case WM_INITMENUPOPUP:
            if (app) {
                if ((HMENU)w_param == App_GetTabsMenu(app)) {
                    App_RebuildTabsMenu(app);
                }
                App_UpdateMenuState(app);
            }
            return 0;

        case WM_COMMAND:
            if (!app) {
                return 0;
            }

            if (HIWORD(w_param) == EN_CHANGE) {
                int index;

                for (index = 0; (size_t)index < app->doc_count; ++index) {
                    if ((HWND)l_param == app->docs[index].edit) {
                        App_SetDocumentDirty(app, index, SendMessage(app->docs[index].edit, EM_GETMODIFY, 0, 0) ? 1 : 0);
                        break;
                    }
                }
                return 0;
            }

            switch (LOWORD(w_param)) {
                case ID_FILE_NEW:
                    if (App_PromptActiveDocumentIfNeeded(app)) {
                        App_InsertDocument(app, TEXT(""), NULL);
                    }
                    return 0;

                case ID_FILE_OPEN:
                    App_HandleOpen(app);
                    return 0;

                case ID_FILE_SAVE:
                    App_SaveDocument(app, app->active_doc, 0);
                    return 0;

                case ID_FILE_SAVE_AS:
                    App_SaveDocument(app, app->active_doc, 1);
                    return 0;

                case ID_FILE_CLOSE_TAB:
                    App_CloseDocument(app, app->active_doc);
                    return 0;

                case ID_FILE_EXIT:
                    SendMessage(window, WM_CLOSE, 0, 0);
                    return 0;

                case ID_EDIT_UNDO:
                case ID_EDIT_CUT:
                case ID_EDIT_COPY:
                case ID_EDIT_PASTE:
                case ID_EDIT_DELETE:
                case ID_EDIT_SELECT_ALL:
                    App_HandleClipboardCommand(app, LOWORD(w_param));
                    App_UpdateMenuState(app);
                    return 0;

                case ID_EDIT_TIME_DATE:
                    App_InsertDateTime(app);
                    return 0;

                case ID_EDIT_WORD_WRAP:
                    app->word_wrap = !app->word_wrap;
                    App_RecreateEditors(app);
                    App_UpdateMenuState(app);
                    return 0;

                case ID_EDIT_FONT:
                    App_ChooseFont(app);
                    return 0;

                case ID_SEARCH_FIND:
                    App_ShowFindDialog(app);
                    return 0;

                case ID_SEARCH_FIND_NEXT:
                    App_FindInActiveDocument(app, app->find_reverse, app->find_match_case);
                    return 0;

                case ID_TABS_NEXT:
                    App_SwitchDocument(app, 1);
                    return 0;

                case ID_TABS_PREVIOUS:
                    App_SwitchDocument(app, -1);
                    return 0;

                case ID_HELP_ABOUT:
                    App_ShowAbout(app);
                    return 0;
            }

            if (LOWORD(w_param) >= ID_TABS_DOCUMENT_FIRST && (size_t)(LOWORD(w_param) - ID_TABS_DOCUMENT_FIRST) < app->doc_count) {
                App_SelectDocument(app, LOWORD(w_param) - ID_TABS_DOCUMENT_FIRST);
                return 0;
            }
            return 0;

        case WM_NOTIFY:
            if (app && ((LPNMHDR)l_param)->idFrom == IDC_TAB) {
                if (((LPNMHDR)l_param)->code == TCN_SELCHANGE) {
                    int selection;

                    selection = TabCtrl_GetCurSel(app->tab);
                    if (selection >= 0 && (size_t)selection < app->doc_count) {
                        App_SelectDocument(app, selection);
                    }
                }
            }
            return 0;

        case WM_CLOSE:
            if (!app || App_CloseAllDocuments(app)) {
                DestroyWindow(window);
            }
            return 0;

        case WM_DESTROY:
            if (app) {
                int index;

                for (index = 0; (size_t)index < app->doc_count; ++index) {
                    if (app->docs[index].edit) {
                        DestroyWindow(app->docs[index].edit);
                    }
                }

                free(app->docs);
                app->docs = NULL;
                app->doc_count = 0;
                app->doc_capacity = 0;

                if (app->accelerator) {
                    DestroyAcceleratorTable(app->accelerator);
                    app->accelerator = NULL;
                }

                if (app->edit_font && app->owns_edit_font) {
                    DeleteObject(app->edit_font);
                    app->edit_font = NULL;
                    app->owns_edit_font = 0;
                }

                if (app->command_bar) {
                    CommandBar_Destroy(app->command_bar);
                    app->command_bar = NULL;
                }

                if (app->menu) {
                    DestroyMenu(app->menu);
                    app->menu = NULL;
                }
            }
            PostQuitMessage(0);
            return 0;
    }

    return DefWindowProc(window, message, w_param, l_param);
}

static int App_RegisterClass(HINSTANCE instance) {
    WNDCLASS window_class;
    WNDCLASS find_class;

    memset(&window_class, 0, sizeof(window_class));
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = App_WindowProc;
    window_class.hInstance = instance;
    window_class.hIcon = LoadIcon(instance, MAKEINTRESOURCE(IDI_APP));
    window_class.hCursor = LoadCursor(NULL, IDC_IBEAM);
    window_class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    window_class.lpszClassName = APP_CLASS_NAME;
    if (!RegisterClass(&window_class)) {
        return 0;
    }

    memset(&find_class, 0, sizeof(find_class));
    find_class.style = CS_HREDRAW | CS_VREDRAW;
    find_class.lpfnWndProc = App_FindWindowProc;
    find_class.hInstance = instance;
    find_class.hCursor = LoadCursor(NULL, IDC_ARROW);
    find_class.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    find_class.lpszClassName = FIND_CLASS_NAME;
    return RegisterClass(&find_class) != 0;
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous_instance, LPWSTR command_line, int show_command) {
    INITCOMMONCONTROLSEX controls;
    APP_STATE app;
    HICON app_icon;
    HWND window;
    MSG message;

    (void)previous_instance;
    (void)command_line;

    memset(&controls, 0, sizeof(controls));
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_TAB_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&controls);

    if (!App_RegisterClass(instance)) {
        MessageBox(NULL, TEXT("Window class registration failed."), APP_TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }

    memset(&app, 0, sizeof(app));
    app.instance = instance;
    app.ui_font = App_GetUiFont();
    app.word_wrap = 1;
    app.next_untitled_id = 1;
    app.accelerator = App_CreateAccelerators();

    window = CreateWindow(
        APP_CLASS_NAME,
        APP_TITLE,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        620,
        350,
        NULL,
        NULL,
        instance,
        &app
    );

    if (!window) {
        MessageBox(NULL, TEXT("Main window creation failed."), APP_TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }

    app_icon = LoadIcon(instance, MAKEINTRESOURCE(IDI_APP));
    if (app_icon) {
        SendMessage(window, WM_SETICON, ICON_BIG, (LPARAM)app_icon);
        SendMessage(window, WM_SETICON, ICON_SMALL, (LPARAM)app_icon);
    }

    ShowWindow(window, show_command);
    UpdateWindow(window);

    while (GetMessage(&message, NULL, 0, 0)) {
        HWND focused_window;
        HWND active_edit;

        focused_window = GetFocus();
        active_edit = App_GetActiveEdit(&app);
        if (
            message.message == WM_KEYDOWN &&
            message.wParam == VK_RETURN &&
            active_edit &&
            (App_IsEditorKeyTarget(&app, message.hwnd) || App_IsEditorKeyTarget(&app, focused_window))
        ) {
            SendMessage(active_edit, EM_REPLACESEL, TRUE, (LPARAM)TEXT("\r\n"));
            continue;
        }

        if (!app.accelerator || !TranslateAccelerator(window, app.accelerator, &message)) {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
    }

    return (int)message.wParam;
}
