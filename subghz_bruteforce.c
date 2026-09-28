#include "signal_tx.h"

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <gui/elements.h>
#include <dialogs/dialogs.h>
#include <storage/storage.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>

#include <string.h>
#include <stdlib.h>

#define TAG              "SigBf"
#define BROWSE_ROOT      EXT_PATH("")
#define MAX_NAME_LEN     256
#define MAX_FILES        2000
#define EMULATE_MIN_MS   1500 // minimum dwell time for emulation formats (RFID/iBtn/NFC)

typedef enum {
    SubGhzBfViewMenu,
    SubGhzBfViewSettings,
    SubGhzBfViewRun,
    SubGhzBfViewAbout,
} SubGhzBfViewId;

typedef enum {
    SubGhzBfMenuSelectFolder,
    SubGhzBfMenuStart,
    SubGhzBfMenuSettings,
    SubGhzBfMenuAbout,
    SubGhzBfMenuCredit,
} SubGhzBfMenuItem;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* submenu;
    VariableItemList* settings_list;
    Widget* about;
    View* run_view;

    Storage* storage;
    DialogsApp* dialogs;
    NotificationApp* notifications;
    SignalTx* tx;

    FuriString* folder;
    uint32_t file_count;

    // settings
    uint32_t delay_ms;
    uint32_t repeats;
    bool loop;

    // worker
    FuriThread* worker;
    volatile bool worker_running;
    volatile bool worker_stop;
    volatile bool worker_pause;
    volatile bool worker_abort; // abort the in-flight transmission (stop or skip)
    volatile int worker_skip; // -1 = previous, +1 = next, 0 = none
} SubGhzBfApp;

typedef struct {
    bool scanning;
    bool running;
    bool paused;
    bool done;
    uint32_t total;
    uint32_t current;
    uint32_t sent_ok;
    uint32_t errors;
    uint32_t cycle;
    char type[8];
    char filename[64];
    char info[48];
    char status[24];
} SubGhzBfRunModel;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool subghz_bf_is_signal(const char* name) {
    return signal_tx_type_from_name(name) != SignalTypeUnknown;
}

static uint32_t subghz_bf_count_files(SubGhzBfApp* app) {
    uint32_t count = 0;
    if(furi_string_size(app->folder) == 0) return 0;

    File* dir = storage_file_alloc(app->storage);
    char name[MAX_NAME_LEN];
    FileInfo fileinfo;
    if(storage_dir_open(dir, furi_string_get_cstr(app->folder))) {
        while(storage_dir_read(dir, &fileinfo, name, sizeof(name))) {
            if(file_info_is_dir(&fileinfo)) continue;
            if(subghz_bf_is_signal(name)) count++;
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    return count;
}

static void subghz_bf_worker_join(SubGhzBfApp* app) {
    if(app->worker) {
        app->worker_stop = true;
        app->worker_abort = true; // break any in-flight transmission immediately
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
        app->worker = NULL;
    }
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------

static void subghz_bf_submenu_callback(void* context, uint32_t index);

static void subghz_bf_build_menu(SubGhzBfApp* app) {
    submenu_reset(app->submenu);
    submenu_set_header(app->submenu, "Universal Bruteforce");

    submenu_add_item(
        app->submenu,
        "Select folder",
        SubGhzBfMenuSelectFolder,
        subghz_bf_submenu_callback,
        app);

    char start_label[48];
    if(furi_string_size(app->folder) == 0) {
        snprintf(start_label, sizeof(start_label), "Start (no folder)");
    } else {
        snprintf(start_label, sizeof(start_label), "Start (%lu files)", (unsigned long)app->file_count);
    }
    submenu_add_item(
        app->submenu, start_label, SubGhzBfMenuStart, subghz_bf_submenu_callback, app);

    submenu_add_item(
        app->submenu, "Settings", SubGhzBfMenuSettings, subghz_bf_submenu_callback, app);
    submenu_add_item(app->submenu, "About", SubGhzBfMenuAbout, subghz_bf_submenu_callback, app);
    submenu_add_item(
        app->submenu, "by @musicandmyth", SubGhzBfMenuCredit, subghz_bf_submenu_callback, app);
}

static void subghz_bf_show_message(SubGhzBfApp* app, const char* header, const char* text) {
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, header, 64, 4, AlignCenter, AlignTop);
    dialog_message_set_text(message, text, 64, 32, AlignCenter, AlignCenter);
    dialog_message_set_buttons(message, NULL, "OK", NULL);
    dialog_message_show(app->dialogs, message);
    dialog_message_free(message);
}

static void subghz_bf_select_folder(SubGhzBfApp* app) {
    DialogsFileBrowserOptions options;
    // No extension filter: show every file so mixed-format folders are visible.
    dialog_file_browser_set_basic_options(&options, "", NULL);
    options.base_path = BROWSE_ROOT;
    options.select_right = true; // allow selecting a directory with the right key

    FuriString* start = furi_string_alloc();
    if(furi_string_size(app->folder) > 0) {
        furi_string_set(start, app->folder);
    } else {
        furi_string_set(start, BROWSE_ROOT);
    }
    FuriString* result = furi_string_alloc();

    if(dialog_file_browser_show(app->dialogs, result, start, &options)) {
        FileInfo fileinfo;
        if(storage_common_stat(app->storage, furi_string_get_cstr(result), &fileinfo) == FSE_OK &&
           file_info_is_dir(&fileinfo)) {
            furi_string_set(app->folder, result);
        } else {
            // A file was picked - use its parent folder.
            furi_string_set(app->folder, result);
            size_t slash = furi_string_search_rchar(app->folder, '/', 0);
            if(slash != FURI_STRING_FAILURE) {
                furi_string_left(app->folder, slash);
            }
        }
        app->file_count = subghz_bf_count_files(app);
        subghz_bf_build_menu(app);
    }

    furi_string_free(start);
    furi_string_free(result);
}

static void subghz_bf_start_attack(SubGhzBfApp* app);

static void subghz_bf_submenu_callback(void* context, uint32_t index) {
    SubGhzBfApp* app = context;
    switch(index) {
    case SubGhzBfMenuSelectFolder:
        subghz_bf_select_folder(app);
        break;
    case SubGhzBfMenuStart:
        subghz_bf_start_attack(app);
        break;
    case SubGhzBfMenuSettings:
        view_dispatcher_switch_to_view(app->view_dispatcher, SubGhzBfViewSettings);
        break;
    case SubGhzBfMenuAbout:
    case SubGhzBfMenuCredit:
        view_dispatcher_switch_to_view(app->view_dispatcher, SubGhzBfViewAbout);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

static const uint32_t delay_values[] = {0, 100, 250, 500, 1000, 2000, 5000};
static const char* const delay_names[] = {"0ms", "100ms", "250ms", "500ms", "1s", "2s", "5s"};
#define DELAY_COUNT (sizeof(delay_values) / sizeof(delay_values[0]))

static void subghz_bf_delay_changed(VariableItem* item) {
    SubGhzBfApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->delay_ms = delay_values[idx];
    variable_item_set_current_value_text(item, delay_names[idx]);
}

#define REPEATS_COUNT 10

static void subghz_bf_repeats_changed(VariableItem* item) {
    SubGhzBfApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->repeats = idx + 1;
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", (unsigned int)(idx + 1));
    variable_item_set_current_value_text(item, buf);
}

static void subghz_bf_loop_changed(VariableItem* item) {
    SubGhzBfApp* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    app->loop = (idx == 1);
    variable_item_set_current_value_text(item, idx ? "On" : "Off");
}

static void subghz_bf_build_settings(SubGhzBfApp* app) {
    VariableItem* item;

    item = variable_item_list_add(
        app->settings_list, "Delay between", DELAY_COUNT, subghz_bf_delay_changed, app);
    uint8_t delay_idx = 3; // 500ms default
    for(uint8_t i = 0; i < DELAY_COUNT; i++) {
        if(delay_values[i] == app->delay_ms) {
            delay_idx = i;
            break;
        }
    }
    variable_item_set_current_value_index(item, delay_idx);
    variable_item_set_current_value_text(item, delay_names[delay_idx]);

    item = variable_item_list_add(
        app->settings_list, "Repeats/file", REPEATS_COUNT, subghz_bf_repeats_changed, app);
    variable_item_set_current_value_index(item, app->repeats - 1);
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", (unsigned int)app->repeats);
    variable_item_set_current_value_text(item, buf);

    item =
        variable_item_list_add(app->settings_list, "Loop forever", 2, subghz_bf_loop_changed, app);
    variable_item_set_current_value_index(item, app->loop ? 1 : 0);
    variable_item_set_current_value_text(item, app->loop ? "On" : "Off");

    // Static credit line at the bottom (0 values = non-interactive label).
    variable_item_list_add(app->settings_list, "by @musicandmyth", 0, NULL, NULL);
}

// ---------------------------------------------------------------------------
// Run view
// ---------------------------------------------------------------------------

static void subghz_bf_run_draw(Canvas* canvas, void* model) {
    SubGhzBfRunModel* m = model;
    char buf[64];

    canvas_clear(canvas);

    // Loading / scanning screen.
    if(m->scanning) {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignCenter, "Scanning folder");
        canvas_set_font(canvas, FontBigNumbers);
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)m->total);
        canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter, buf);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 54, AlignCenter, AlignCenter, "signals found...");
        return;
    }

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Universal Bruteforce");

    canvas_set_font(canvas, FontSecondary);

    if(m->cycle > 1) {
        snprintf(
            buf,
            sizeof(buf),
            "%lu/%lu  loop %lu  OK%lu E%lu",
            (unsigned long)m->current,
            (unsigned long)m->total,
            (unsigned long)m->cycle,
            (unsigned long)m->sent_ok,
            (unsigned long)m->errors);
    } else {
        snprintf(
            buf,
            sizeof(buf),
            "%lu/%lu   OK%lu  Err%lu",
            (unsigned long)m->current,
            (unsigned long)m->total,
            (unsigned long)m->sent_ok,
            (unsigned long)m->errors);
    }
    canvas_draw_str(canvas, 2, 21, buf);

    // Progress bar
    const uint8_t x = 2, y = 24, w = 124, h = 6;
    canvas_draw_frame(canvas, x, y, w, h);
    if(m->total > 0) {
        uint32_t fill = ((uint32_t)(w - 2) * m->current) / m->total;
        if(fill > (uint32_t)(w - 2)) fill = w - 2;
        if(fill > 0) canvas_draw_box(canvas, x + 1, y + 1, (uint8_t)fill, h - 2);
    }

    // Current file name
    canvas_draw_str(canvas, 2, 39, m->filename[0] ? m->filename : "-");

    // Signal type + info (e.g. "SubGHz 433.92 Princeton", "IR 3 signal(s)").
    snprintf(buf, sizeof(buf), "%s %s", m->type[0] ? m->type : "-", m->info);
    canvas_draw_str(canvas, 2, 49, buf);

    // Bottom row: navigation hints / status.
    if(m->done) {
        snprintf(buf, sizeof(buf), "%s - Back to exit", m->status);
        canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, buf);
    } else {
        elements_button_left(canvas, "Prev");
        elements_button_center(canvas, m->paused ? "Resume" : "Pause");
        elements_button_right(canvas, "Next");
    }
}

static bool subghz_bf_run_input(InputEvent* event, void* context) {
    SubGhzBfApp* app = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    switch(event->key) {
    case InputKeyBack:
        if(event->type != InputTypeShort) return false;
        subghz_bf_worker_join(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, SubGhzBfViewMenu);
        return true;

    case InputKeyOk:
        if(event->type != InputTypeShort) return false;
        if(app->worker_running) {
            app->worker_pause = !app->worker_pause;
            bool paused = app->worker_pause;
            with_view_model(
                app->run_view, SubGhzBfRunModel * m, { m->paused = paused; }, true);
        }
        return true;

    case InputKeyRight:
        if(app->worker_running) {
            app->worker_skip = 1;
            app->worker_abort = true;
        }
        return true;

    case InputKeyLeft:
        if(app->worker_running) {
            app->worker_skip = -1;
            app->worker_abort = true;
        }
        return true;

    default:
        return false;
    }
}

static uint32_t subghz_bf_run_previous(void* context) {
    UNUSED(context);
    return SubGhzBfViewMenu;
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

// Growable list of file names (built during the scan phase).
typedef struct {
    char** items;
    size_t count;
    size_t cap;
} SubGhzBfNameList;

static void subghz_bf_namelist_add(SubGhzBfNameList* list, const char* name) {
    if(list->count >= MAX_FILES) return;
    if(list->count == list->cap) {
        size_t newcap = list->cap ? list->cap * 2 : 32;
        char** grown = realloc(list->items, newcap * sizeof(char*));
        if(!grown) return;
        list->items = grown;
        list->cap = newcap;
    }
    size_t len = strlen(name);
    char* copy = malloc(len + 1);
    if(!copy) return;
    memcpy(copy, name, len + 1);
    list->items[list->count++] = copy;
}

static void subghz_bf_namelist_free(SubGhzBfNameList* list) {
    for(size_t i = 0; i < list->count; i++) free(list->items[i]);
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

// Insertion sort by filename. The C library qsort() is present in the
// firmware's symbol table but disabled, so an app that references it fails the
// FAP API check; we sort in-place instead.
static void subghz_bf_sort_names(char** items, size_t count) {
    for(size_t i = 1; i < count; i++) {
        char* key = items[i];
        size_t j = i;
        while(j > 0 && strcmp(items[j - 1], key) > 0) {
            items[j] = items[j - 1];
            j--;
        }
        items[j] = key;
    }
}

// Build the sorted list of signal files, updating the loading screen as it goes.
static void subghz_bf_scan(SubGhzBfApp* app, SubGhzBfNameList* list) {
    File* dir = storage_file_alloc(app->storage);
    char name[MAX_NAME_LEN];
    FileInfo fileinfo;

    if(storage_dir_open(dir, furi_string_get_cstr(app->folder))) {
        while(storage_dir_read(dir, &fileinfo, name, sizeof(name))) {
            if(app->worker_stop) break;
            if(file_info_is_dir(&fileinfo)) continue;
            if(!subghz_bf_is_signal(name)) continue;
            subghz_bf_namelist_add(list, name);

            uint32_t found = list->count;
            with_view_model(
                app->run_view, SubGhzBfRunModel * m, { m->total = found; }, true);
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);

    if(list->count > 1) {
        subghz_bf_sort_names(list->items, list->count);
    }
}

// Sleep for `ms`, waking early if the run is stopped or the user skips.
static bool subghz_bf_wait(SubGhzBfApp* app, uint32_t ms) {
    uint32_t waited = 0;
    while(waited < ms) {
        if(app->worker_stop || app->worker_skip != 0) return false;
        furi_delay_ms(10);
        waited += 10;
    }
    return true;
}

// Apply a pending skip to the index, wrapping/clamping. Returns false if the
// run should finish (skipped past the end with looping disabled).
static bool subghz_bf_apply_skip(SubGhzBfApp* app, int* i, int count) {
    *i += app->worker_skip;
    app->worker_skip = 0;
    if(*i < 0) *i = 0;
    if(*i >= count) {
        if(app->loop) {
            *i = 0;
        } else {
            return false;
        }
    }
    return true;
}

static int32_t subghz_bf_worker(void* context) {
    SubGhzBfApp* app = context;

    // Phase 1: scan the folder (loading screen).
    SubGhzBfNameList list = {0};
    subghz_bf_scan(app, &list);

    if(list.count == 0 || app->worker_stop) {
        bool stopped = app->worker_stop;
        uint32_t total = list.count;
        with_view_model(
            app->run_view,
            SubGhzBfRunModel * m,
            {
                m->scanning = false;
                m->running = false;
                m->done = true;
                m->total = total;
                strncpy(m->status, stopped ? "Stopped" : "No files", sizeof(m->status) - 1);
                m->status[sizeof(m->status) - 1] = '\0';
            },
            true);
        subghz_bf_namelist_free(&list);
        app->worker_running = false;
        return 0;
    }

    uint32_t total = list.count;
    with_view_model(
        app->run_view, SubGhzBfRunModel * m, { m->scanning = false; m->total = total; }, true);

    // Phase 2: transmit each signal.
    signal_tx_session_begin(app->tx);
    notification_message(app->notifications, &sequence_blink_start_blue);

    uint32_t cycle = 0;
    bool aborted = false;

    do {
        cycle++;
        int i = 0;
        while(i < (int)list.count) {
            if(app->worker_stop) {
                aborted = true;
                break;
            }

            // Honor pause; a skip or stop breaks out of it.
            while(app->worker_pause && !app->worker_stop && app->worker_skip == 0) {
                furi_delay_ms(50);
            }
            if(app->worker_stop) {
                aborted = true;
                break;
            }
            if(app->worker_skip != 0) {
                if(!subghz_bf_apply_skip(app, &i, (int)list.count)) break;
                continue;
            }

            const char* name = list.items[i];
            FuriString* path =
                furi_string_alloc_printf("%s/%s", furi_string_get_cstr(app->folder), name);

            SignalType type = signal_tx_type_from_name(name);
            const char* type_str = signal_tx_type_str(type);
            bool emulation = signal_tx_is_emulation(type);

            uint32_t idx1 = (uint32_t)i + 1;
            with_view_model(
                app->run_view,
                SubGhzBfRunModel * m,
                {
                    m->current = idx1;
                    m->cycle = cycle;
                    strncpy(m->filename, name, sizeof(m->filename) - 1);
                    m->filename[sizeof(m->filename) - 1] = '\0';
                    strncpy(m->type, type_str, sizeof(m->type) - 1);
                    m->type[sizeof(m->type) - 1] = '\0';
                },
                true);

            // Emulation formats hold the field for a dwell time; one-shot formats
            // fire `repeats` times. The abort flag ends either early.
            uint32_t dwell = app->delay_ms < EMULATE_MIN_MS ? EMULATE_MIN_MS : app->delay_ms;
            uint32_t passes = emulation ? 1 : app->repeats;

            SignalTxInfo txinfo = {0};
            SignalTxResult res = SignalTxOk;
            for(uint32_t r = 0; r < passes; r++) {
                if(app->worker_stop || app->worker_skip != 0) break;
                app->worker_abort = false;
                res = signal_tx_play(
                    app->tx, type, furi_string_get_cstr(path), dwell, &txinfo, &app->worker_abort);
                if(res != SignalTxOk) break;
                if(r + 1 < passes && app->delay_ms) {
                    if(!subghz_bf_wait(app, app->delay_ms)) break;
                }
            }

            with_view_model(
                app->run_view,
                SubGhzBfRunModel * m,
                {
                    strncpy(m->info, txinfo.label, sizeof(m->info) - 1);
                    m->info[sizeof(m->info) - 1] = '\0';
                    if(res == SignalTxOk) {
                        m->sent_ok++;
                    } else if(res != SignalTxStopped) {
                        m->errors++;
                    }
                    strncpy(m->status, signal_tx_result_str(res), sizeof(m->status) - 1);
                    m->status[sizeof(m->status) - 1] = '\0';
                },
                true);

            furi_string_free(path);

            if(app->worker_stop) {
                aborted = true;
                break;
            }
            if(app->worker_skip != 0) {
                if(!subghz_bf_apply_skip(app, &i, (int)list.count)) break;
                continue;
            }

            // Delay between files (a skip interrupts it and is applied at the top).
            if(app->delay_ms && !subghz_bf_wait(app, app->delay_ms)) {
                if(app->worker_stop) {
                    aborted = true;
                    break;
                }
                continue; // skip pressed during the delay
            }

            i++;
        }

        if(aborted) break;
    } while(app->loop && !app->worker_stop);

    notification_message(app->notifications, &sequence_blink_stop);
    signal_tx_session_end(app->tx);

    with_view_model(
        app->run_view,
        SubGhzBfRunModel * m,
        {
            m->running = false;
            m->paused = false;
            m->done = true;
            strncpy(m->status, aborted ? "Stopped" : "Done", sizeof(m->status) - 1);
            m->status[sizeof(m->status) - 1] = '\0';
        },
        true);

    subghz_bf_namelist_free(&list);
    app->worker_running = false;
    return 0;
}

static void subghz_bf_start_attack(SubGhzBfApp* app) {
    if(furi_string_size(app->folder) == 0 || app->file_count == 0) {
        subghz_bf_show_message(
            app,
            "No signals",
            "Select a folder with\n.sub/.ir/.rfid/.ibtn/.nfc\nfiles first.");
        return;
    }

    subghz_bf_worker_join(app);

    app->worker_stop = false;
    app->worker_pause = false;
    app->worker_abort = false;
    app->worker_skip = 0;
    app->worker_running = true;

    with_view_model(
        app->run_view,
        SubGhzBfRunModel * m,
        {
            memset(m, 0, sizeof(SubGhzBfRunModel));
            m->scanning = true;
            m->running = true;
            strncpy(m->status, "Scanning", sizeof(m->status) - 1);
        },
        true);

    app->worker = furi_thread_alloc_ex("UniBfWorker", 8192, subghz_bf_worker, app);
    furi_thread_start(app->worker);

    view_dispatcher_switch_to_view(app->view_dispatcher, SubGhzBfViewRun);
}

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------

static uint32_t subghz_bf_exit_callback(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static uint32_t subghz_bf_back_to_menu(void* context) {
    UNUSED(context);
    return SubGhzBfViewMenu;
}

static SubGhzBfApp* subghz_bf_app_alloc(void) {
    SubGhzBfApp* app = malloc(sizeof(SubGhzBfApp));
    memset(app, 0, sizeof(SubGhzBfApp));

    app->folder = furi_string_alloc();
    app->delay_ms = 500;
    app->repeats = 1;
    app->loop = false;

    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->dialogs = furi_record_open(RECORD_DIALOGS);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    app->tx = signal_tx_alloc();

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);

    // Menu
    app->submenu = submenu_alloc();
    subghz_bf_build_menu(app);
    view_set_previous_callback(submenu_get_view(app->submenu), subghz_bf_exit_callback);
    view_dispatcher_add_view(
        app->view_dispatcher, SubGhzBfViewMenu, submenu_get_view(app->submenu));

    // Settings
    app->settings_list = variable_item_list_alloc();
    subghz_bf_build_settings(app);
    view_set_previous_callback(
        variable_item_list_get_view(app->settings_list), subghz_bf_back_to_menu);
    view_dispatcher_add_view(
        app->view_dispatcher,
        SubGhzBfViewSettings,
        variable_item_list_get_view(app->settings_list));

    // Run view
    app->run_view = view_alloc();
    view_set_context(app->run_view, app);
    view_allocate_model(app->run_view, ViewModelTypeLocking, sizeof(SubGhzBfRunModel));
    view_set_draw_callback(app->run_view, subghz_bf_run_draw);
    view_set_input_callback(app->run_view, subghz_bf_run_input);
    view_set_previous_callback(app->run_view, subghz_bf_run_previous);
    view_dispatcher_add_view(app->view_dispatcher, SubGhzBfViewRun, app->run_view);

    // About
    app->about = widget_alloc();
    widget_add_text_scroll_element(
        app->about,
        0,
        0,
        128,
        64,
        "\e#Universal Bruteforce\e#\n"
        "Plays every signal file in a\n"
        "chosen folder, one by one -\n"
        "a dictionary attack across\n"
        "formats.\n\n"
        "Supports:\n"
        ".sub  Sub-GHz (transmit)\n"
        ".ir   Infrared (transmit)\n"
        ".rfid LF RFID (emulate)\n"
        ".ibtn iButton (emulate)\n"
        ".nfc  NFC (emulate)\n\n"
        "1. Select folder\n"
        "2. Set delay/repeats/loop\n"
        "3. Start\n\n"
        "IR files fire every signal\n"
        "they contain. Emulated types\n"
        "(RFID/iBtn/NFC) hold each for\n"
        "the delay time (min 1.5s).\n\n"
        "Left/Right = skip, OK = pause,\n"
        "Back = stop.\n\n"
        "Only transmit or emulate on\n"
        "devices you are legally\n"
        "authorized to operate.");
    view_set_previous_callback(widget_get_view(app->about), subghz_bf_back_to_menu);
    view_dispatcher_add_view(
        app->view_dispatcher, SubGhzBfViewAbout, widget_get_view(app->about));

    view_dispatcher_switch_to_view(app->view_dispatcher, SubGhzBfViewMenu);
    return app;
}

static void subghz_bf_app_free(SubGhzBfApp* app) {
    subghz_bf_worker_join(app);

    view_dispatcher_remove_view(app->view_dispatcher, SubGhzBfViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, SubGhzBfViewSettings);
    view_dispatcher_remove_view(app->view_dispatcher, SubGhzBfViewRun);
    view_dispatcher_remove_view(app->view_dispatcher, SubGhzBfViewAbout);

    submenu_free(app->submenu);
    variable_item_list_free(app->settings_list);
    view_free(app->run_view);
    widget_free(app->about);
    view_dispatcher_free(app->view_dispatcher);

    signal_tx_free(app->tx);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_DIALOGS);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);

    furi_string_free(app->folder);
    free(app);
}

int32_t subghz_bruteforce_app(void* p) {
    UNUSED(p);
    SubGhzBfApp* app = subghz_bf_app_alloc();
    view_dispatcher_run(app->view_dispatcher);
    subghz_bf_app_free(app);
    return 0;
}
