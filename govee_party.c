/* Govee Party: a dance party (and a B-Gone) for nearby Govee BLE lights,
 * triggered from a Flipper Zero. */

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <notification/notification_messages.h>
#include <stdlib.h>
#include <string.h>

#include "govee_protocol.h"
#include "govee_transport.h"

#define TAG "GoveeParty"

typedef enum {
    ViewMenu,
    ViewStatus,
    ViewSettings,
    ViewPick,
    ViewAbout,
} ViewId;

typedef enum {
    MenuParty,
    MenuLive,
    MenuBGone,
    MenuSettings,
    MenuAbout,
} MenuIndex;

typedef enum {
    JobNone,
    JobParty,
    JobBGone,
    JobScanForLive,
    JobLive,
} Job;

typedef enum {
    EventJobDone = 100,
    EventStatusChanged,
} CustomEvent;

typedef enum {
    PatternRainbow,
    PatternStrobe,
    PatternPolice,
    PatternDisco,
    PatternPulse,
    PatternFire,
    PatternCount,
} Pattern;

static const char* const PATTERN_NAMES[PatternCount] =
    {"Rainbow", "Strobe", "Police", "Disco", "Pulse", "Fire"};

typedef struct {
    char title[24];
    char line1[40];
    char line2[40];
    char hint[32];
    uint8_t r, g, b; /* current colour, drawn as a swatch in live mode */
    bool show_swatch;
    uint8_t progress; /* 0-100, 255 = hidden */
} StatusModel;

typedef struct {
    Gui* gui;
    NotificationApp* notifications;
    ViewDispatcher* dispatcher;
    Submenu* menu;
    Submenu* pick;
    VariableItemList* settings;
    Widget* about;
    View* status;

    /* settings */
    GoveeTransportKind transport_kind;
    uint8_t scan_seconds;
    bool bgone_persistent;
    bool flipper_led;

    /* worker */
    FuriThread* worker;
    Job job;
    volatile bool stop;
    GoveeTransport* transport;
    GoveeLight lights[GOVEE_MAX_LIGHTS];
    size_t light_count;
    size_t live_index;
    volatile Pattern pattern;
    volatile uint8_t speed; /* 1-5 */
    volatile bool hold;

    /* notification_message() is asynchronous, so the sequence must outlive
     * the call: keep it in the app rather than on the stack. */
    NotificationMessage led_msg[3];
    const NotificationMessage* led_seq[4];
} App;

/* ---------------------------------------------------------------- status view */

static void status_draw(Canvas* canvas, void* model) {
    StatusModel* m = model;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 2, AlignCenter, AlignTop, m->title);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 20, AlignCenter, AlignTop, m->line1);
    canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignTop, m->line2);
    if(m->show_swatch) {
        /* a bar whose fill height follows perceived brightness */
        uint16_t lum = (m->r * 3 + m->g * 6 + m->b) / 10;
        uint8_t h = (uint8_t)(lum * 10 / 255);
        canvas_draw_frame(canvas, 4, 44, 120, 12);
        if(h) canvas_draw_box(canvas, 6, 46 + (10 - h) / 2, 116, h ? h : 1);
    } else if(m->progress != 255) {
        canvas_draw_frame(canvas, 14, 46, 100, 8);
        canvas_draw_box(canvas, 15, 47, (uint8_t)(m->progress * 98 / 100), 6);
    }
    canvas_draw_str_aligned(canvas, 64, 63, AlignCenter, AlignBottom, m->hint);
}

static bool status_input(InputEvent* event, void* context) {
    App* app = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;
    if(app->job != JobLive) return false;
    switch(event->key) {
    case InputKeyLeft:
        app->pattern = (app->pattern + PatternCount - 1) % PatternCount;
        return true;
    case InputKeyRight:
        app->pattern = (app->pattern + 1) % PatternCount;
        return true;
    case InputKeyUp:
        if(app->speed < 5) app->speed++;
        return true;
    case InputKeyDown:
        if(app->speed > 1) app->speed--;
        return true;
    case InputKeyOk:
        app->hold = !app->hold;
        return true;
    default:
        return false;
    }
}

static void status_set(App* app, const char* title, const char* line1, const char* line2, const char* hint) {
    with_view_model(
        app->status,
        StatusModel * m,
        {
            if(title) strlcpy(m->title, title, sizeof(m->title));
            if(line1) strlcpy(m->line1, line1, sizeof(m->line1));
            if(line2) strlcpy(m->line2, line2, sizeof(m->line2));
            if(hint) strlcpy(m->hint, hint, sizeof(m->hint));
        },
        true);
}

static void status_progress(App* app, uint8_t progress) {
    with_view_model(app->status, StatusModel * m, { m->progress = progress; }, true);
}

static void status_swatch(App* app, bool show, uint8_t r, uint8_t g, uint8_t b) {
    with_view_model(
        app->status,
        StatusModel * m,
        {
            m->show_swatch = show;
            m->r = r;
            m->g = g;
            m->b = b;
        },
        true);
}

/* ------------------------------------------------------------ Flipper LED */

static void flipper_led(App* app, uint8_t r, uint8_t g, uint8_t b) {
    if(!app->flipper_led) return;
    app->led_msg[0].data.led.value = r;
    app->led_msg[1].data.led.value = g;
    app->led_msg[2].data.led.value = b;
    notification_message(app->notifications, (const NotificationSequence*)app->led_seq);
}

static void flipper_led_off(App* app) {
    notification_message(app->notifications, &sequence_reset_rgb);
}

/* ------------------------------------------------------------------ worker */

static void on_light_found(const GoveeLight* light, size_t count, void* context) {
    App* app = context;
    char l1[40], l2[40];
    snprintf(l1, sizeof(l1), "Found %u light%s", (unsigned)count, count == 1 ? "" : "s");
    snprintf(l2, sizeof(l2), "%s", light->name);
    status_set(app, NULL, l1, l2, NULL);
    notification_message(app->notifications, &sequence_blink_cyan_10);
}

static bool worker_scan(App* app, const char* title) {
    status_set(app, title, "Scanning...", "", "Back = cancel");
    status_progress(app, 255);
    app->light_count = 0;
    bool ok = govee_transport_scan(
        app->transport,
        app->scan_seconds * 1000u,
        app->lights,
        GOVEE_MAX_LIGHTS,
        &app->light_count,
        &app->stop,
        on_light_found,
        app);
    if(!ok && app->light_count == 0) {
        status_set(app, NULL, "Scan failed", govee_transport_last_error(app->transport), "Back");
        return false;
    }
    return true;
}

static void worker_send(App* app, const uint8_t* frame) {
    govee_transport_write(app->transport, frame, true);
    furi_delay_ms(60);
}

/* Visits every found light, runs `visit`, disconnects. Returns hits. */
typedef void (*VisitFn)(App* app, const GoveeLight* light);

static size_t worker_sweep(App* app, const char* title, VisitFn visit, size_t* failures) {
    size_t hits = 0;
    *failures = 0;
    for(size_t i = 0; i < app->light_count && !app->stop; i++) {
        const GoveeLight* light = &app->lights[i];
        char l1[40];
        snprintf(l1, sizeof(l1), "%u/%u %s", (unsigned)i + 1, (unsigned)app->light_count, light->model);
        status_set(app, title, l1, light->name, "Back = stop");
        status_progress(app, (uint8_t)(i * 100 / app->light_count));
        if(!govee_transport_connect(app->transport, light, 6000)) {
            FURI_LOG_W(TAG, "%s: %s", light->name, govee_transport_last_error(app->transport));
            (*failures)++;
            continue;
        }
        visit(app, light);
        govee_transport_disconnect(app->transport);
        hits++;
        furi_delay_ms(150);
    }
    status_progress(app, 100);
    return hits;
}

static void visit_party(App* app, const GoveeLight* light) {
    uint8_t f[GOVEE_FRAME_LEN];
    govee_frame_power(f, true);
    worker_send(app, f);
    govee_frame_brightness(f, 100, light->dialect);
    worker_send(app, f);
    /* A couple of quick colour pops so it is obvious something happened. */
    static const uint8_t pops[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 0, 255}};
    for(int i = 0; i < 4; i++) {
        govee_frame_rgb(f, pops[i][0], pops[i][1], pops[i][2], light->dialect);
        flipper_led(app, pops[i][0], pops[i][1], pops[i][2]);
        worker_send(app, f);
        furi_delay_ms(90);
    }
    /* Then hand the light to its own party brain: the blinking scene as a
     * fallback, then the microphone-driven "energic" music mode, which the
     * H6159 family understands and which keeps going after we leave. */
    govee_frame_scene(f, GOVEE_SCENE_BLINKING);
    worker_send(app, f);
    govee_frame_music(f, GOVEE_MUSIC_ENERGIC, 255, 0, 255);
    worker_send(app, f);
    flipper_led_off(app);
}

static void visit_bgone(App* app, const GoveeLight* light) {
    UNUSED(light);
    uint8_t f[GOVEE_FRAME_LEN];
    govee_frame_power(f, false);
    worker_send(app, f);
    worker_send(app, f); /* belt and braces */
}

static void worker_live(App* app) {
    const GoveeLight* light = &app->lights[app->live_index];
    char hint[32];
    status_set(app, "Live", "Connecting...", light->name, "");
    status_progress(app, 255);
    if(!govee_transport_connect(app->transport, light, 6000)) {
        status_set(app, NULL, "Connect failed", govee_transport_last_error(app->transport), "Back");
        return;
    }
    uint8_t f[GOVEE_FRAME_LEN];
    govee_frame_power(f, true);
    worker_send(app, f);
    govee_frame_brightness(f, 100, light->dialect);
    worker_send(app, f);

    uint32_t tick = 0;
    uint16_t hue = 0;
    uint8_t r = 0, g = 0, b = 0;
    uint32_t last_keepalive = furi_get_tick();
    while(!app->stop && govee_transport_is_connected(app->transport)) {
        Pattern p = app->pattern;
        uint8_t speed = app->speed;
        snprintf(hint, sizeof(hint), "< %s >  spd %u%s", PATTERN_NAMES[p], speed, app->hold ? " HOLD" : "");
        char l2[40];
        snprintf(l2, sizeof(l2), "%s", light->name);
        status_set(app, "Live", app->hold ? "Holding colour" : "Streaming", l2, hint);

        uint32_t interval = 260 - speed * 40; /* 220..60 ms */
        if(!app->hold) {
            switch(p) {
            case PatternRainbow:
                hue = (hue + 12) % 360;
                govee_hsv_to_rgb(hue, 255, 255, &r, &g, &b);
                break;
            case PatternStrobe:
                r = g = b = (tick & 1) ? 255 : 1;
                break;
            case PatternPolice:
                if((tick / 2) & 1) {
                    r = 255; g = 0; b = 0;
                } else {
                    r = 0; g = 0; b = 255;
                }
                if(tick & 1) r = g = b = 1;
                break;
            case PatternDisco:
                govee_hsv_to_rgb(rand() % 360, 255, 255, &r, &g, &b);
                break;
            case PatternPulse: {
                uint8_t v = (uint8_t)(tick % 20 < 10 ? 30 + tick % 20 * 22 : 30 + (19 - tick % 20) * 22);
                govee_hsv_to_rgb(hue, 255, v, &r, &g, &b);
                if(tick % 20 == 0) hue = (hue + 47) % 360;
                break;
            }
            case PatternFire:
                govee_hsv_to_rgb(rand() % 30, 255, 120 + rand() % 136, &r, &g, &b);
                break;
            default:
                break;
            }
            govee_frame_rgb(f, r, g, b, light->dialect);
            if(!govee_transport_write(app->transport, f, false)) {
                FURI_LOG_W(TAG, "write: %s", govee_transport_last_error(app->transport));
            }
            flipper_led(app, r, g, b);
            status_swatch(app, true, r, g, b);
        } else if(furi_get_tick() - last_keepalive > furi_ms_to_ticks(2000)) {
            govee_frame_keepalive(f);
            govee_transport_write(app->transport, f, false);
            last_keepalive = furi_get_tick();
        }
        tick++;
        furi_delay_ms(interval);
    }
    status_swatch(app, false, 0, 0, 0);
    flipper_led_off(app);
    govee_transport_disconnect(app->transport);
}

static int32_t worker_thread(void* context) {
    App* app = context;
    const char* why = NULL;
    if(!govee_transport_available(app->transport_kind, &why)) {
        status_set(app, "Unavailable", govee_transport_kind_name(app->transport_kind), why, "Back");
        status_progress(app, 255);
        return 0;
    }
    status_set(app, "Starting", govee_transport_kind_name(app->transport_kind), "", "");
    status_progress(app, 255);
    app->transport = govee_transport_alloc(app->transport_kind);
    if(!app->transport) {
        status_set(
            app,
            "Unavailable",
            govee_transport_kind_name(app->transport_kind),
            app->transport_kind == GoveeTransportUart ? "No bridge on pins 13/14" : "Radio not ready",
            "Back");
        return 0;
    }

    size_t failures = 0;
    switch(app->job) {
    case JobParty:
        if(worker_scan(app, "Dance Party")) {
            if(app->light_count == 0) {
                status_set(app, NULL, "No Govee lights nearby", "", "Back");
            } else {
                size_t hits = worker_sweep(app, "Dance Party", visit_party, &failures);
                char l1[40];
                snprintf(l1, sizeof(l1), "%u light%s partying", (unsigned)hits, hits == 1 ? "" : "s");
                status_set(app, "Dance Party", l1, failures ? "some did not connect" : "", "Back");
                notification_message(app->notifications, &sequence_success);
            }
        }
        break;
    case JobBGone:
        do {
            if(!worker_scan(app, "Govee B-Gone")) break;
            if(app->light_count == 0) {
                status_set(app, NULL, app->bgone_persistent ? "Nothing on. Lurking..." : "No Govee lights nearby", "", "Back");
            } else {
                size_t hits = worker_sweep(app, "Govee B-Gone", visit_bgone, &failures);
                char l1[40];
                snprintf(l1, sizeof(l1), "%u light%s silenced", (unsigned)hits, hits == 1 ? "" : "s");
                status_set(app, "Govee B-Gone", l1, failures ? "some did not connect" : "", app->bgone_persistent ? "Back = stop" : "Back");
                notification_message(app->notifications, &sequence_success);
            }
            if(app->bgone_persistent) {
                for(int i = 0; i < 40 && !app->stop; i++) furi_delay_ms(100);
            }
        } while(app->bgone_persistent && !app->stop);
        break;
    case JobScanForLive:
        worker_scan(app, "Live");
        break;
    case JobLive:
        worker_live(app);
        break;
    default:
        break;
    }

    govee_transport_free(app->transport);
    app->transport = NULL;
    view_dispatcher_send_custom_event(app->dispatcher, EventJobDone);
    return 0;
}

static void worker_start(App* app, Job job) {
    furi_check(app->worker == NULL);
    app->job = job;
    app->stop = false;
    app->worker = furi_thread_alloc_ex("GoveeWorker", 3 * 1024, worker_thread, app);
    furi_thread_start(app->worker);
}

static void worker_stop(App* app) {
    if(!app->worker) return;
    app->stop = true;
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);
    app->worker = NULL;
    app->job = JobNone;
}

/* --------------------------------------------------------------------- UI */

static void pick_callback(void* context, uint32_t index) {
    App* app = context;
    worker_stop(app);
    app->live_index = index;
    app->hold = false;
    worker_start(app, JobLive);
    view_dispatcher_switch_to_view(app->dispatcher, ViewStatus);
}

static void show_pick_list(App* app) {
    submenu_reset(app->pick);
    submenu_set_header(app->pick, app->light_count ? "Pick a light" : "No lights found");
    for(size_t i = 0; i < app->light_count; i++) {
        char label[40];
        snprintf(label, sizeof(label), "%s %ddB", app->lights[i].name, app->lights[i].rssi);
        submenu_add_item(app->pick, label, i, pick_callback, app);
    }
    view_dispatcher_switch_to_view(app->dispatcher, ViewPick);
}

static void menu_callback(void* context, uint32_t index) {
    App* app = context;
    switch(index) {
    case MenuParty:
        worker_start(app, JobParty);
        view_dispatcher_switch_to_view(app->dispatcher, ViewStatus);
        break;
    case MenuLive:
        worker_start(app, JobScanForLive);
        view_dispatcher_switch_to_view(app->dispatcher, ViewStatus);
        break;
    case MenuBGone:
        worker_start(app, JobBGone);
        view_dispatcher_switch_to_view(app->dispatcher, ViewStatus);
        break;
    case MenuSettings:
        view_dispatcher_switch_to_view(app->dispatcher, ViewSettings);
        break;
    case MenuAbout:
        view_dispatcher_switch_to_view(app->dispatcher, ViewAbout);
        break;
    }
}

static bool custom_event_callback(void* context, uint32_t event) {
    App* app = context;
    if(event == EventJobDone) {
        Job job = app->job;
        worker_stop(app);
        if(job == JobScanForLive) show_pick_list(app);
        return true;
    }
    return false;
}

static bool navigation_callback(void* context) {
    App* app = context;
    if(app->worker) {
        worker_stop(app);
        flipper_led_off(app);
    }
    view_dispatcher_switch_to_view(app->dispatcher, ViewMenu);
    return true;
}

static uint32_t exit_callback(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static uint32_t menu_previous_callback(void* context) {
    UNUSED(context);
    return ViewMenu;
}

/* settings */

static const char* const SCAN_LABELS[] = {"3 s", "5 s", "8 s", "12 s"};
static const uint8_t SCAN_VALUES[] = {3, 5, 8, 12};

static void setting_transport_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    app->transport_kind = (GoveeTransportKind)i;
    const char* why = NULL;
    bool ok = govee_transport_available(app->transport_kind, &why);
    char label[32];
    snprintf(label, sizeof(label), "%s%s", govee_transport_kind_name(app->transport_kind), ok ? "" : " (n/a)");
    variable_item_set_current_value_text(item, label);
}

static void setting_scan_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    uint8_t i = variable_item_get_current_value_index(item);
    app->scan_seconds = SCAN_VALUES[i];
    variable_item_set_current_value_text(item, SCAN_LABELS[i]);
}

static void setting_bool_text(VariableItem* item, bool on) {
    variable_item_set_current_value_text(item, on ? "ON" : "OFF");
}

static void setting_persistent_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->bgone_persistent = variable_item_get_current_value_index(item);
    setting_bool_text(item, app->bgone_persistent);
}

static void setting_led_changed(VariableItem* item) {
    App* app = variable_item_get_context(item);
    app->flipper_led = variable_item_get_current_value_index(item);
    setting_bool_text(item, app->flipper_led);
}

static void settings_build(App* app) {
    VariableItem* item;
    item = variable_item_list_add(app->settings, "Radio", GoveeTransportCount, setting_transport_changed, app);
    variable_item_set_current_value_index(item, app->transport_kind);
    setting_transport_changed(item);

    item = variable_item_list_add(app->settings, "Scan time", 4, setting_scan_changed, app);
    variable_item_set_current_value_index(item, 1);
    setting_scan_changed(item);

    item = variable_item_list_add(app->settings, "B-Gone lurks", 2, setting_persistent_changed, app);
    variable_item_set_current_value_index(item, app->bgone_persistent);
    setting_bool_text(item, app->bgone_persistent);

    item = variable_item_list_add(app->settings, "Flipper LED", 2, setting_led_changed, app);
    variable_item_set_current_value_index(item, app->flipper_led);
    setting_bool_text(item, app->flipper_led);
}

/* ------------------------------------------------------------- lifecycle */

static App* app_alloc(void) {
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(*app));
    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    /* Prefer the Flipper's own radio when the firmware can do it. */
    const char* why;
    app->transport_kind = govee_transport_available(GoveeTransportNative, &why) ? GoveeTransportNative :
                                                                                 GoveeTransportUart;
    app->scan_seconds = 5;
    app->bgone_persistent = false;
    app->flipper_led = true;
    app->pattern = PatternRainbow;
    app->speed = 3;
    app->led_msg[0].type = NotificationMessageTypeLedRed;
    app->led_msg[1].type = NotificationMessageTypeLedGreen;
    app->led_msg[2].type = NotificationMessageTypeLedBlue;
    for(int i = 0; i < 3; i++) app->led_seq[i] = &app->led_msg[i];
    app->led_seq[3] = NULL;

    app->dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->dispatcher, custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->dispatcher, navigation_callback);
    view_dispatcher_attach_to_gui(app->dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->menu = submenu_alloc();
    submenu_set_header(app->menu, "Govee Party");
    submenu_add_item(app->menu, "Dance Party!", MenuParty, menu_callback, app);
    submenu_add_item(app->menu, "Live DJ (one light)", MenuLive, menu_callback, app);
    submenu_add_item(app->menu, "Govee B-Gone", MenuBGone, menu_callback, app);
    submenu_add_item(app->menu, "Settings", MenuSettings, menu_callback, app);
    submenu_add_item(app->menu, "About", MenuAbout, menu_callback, app);
    view_set_previous_callback(submenu_get_view(app->menu), exit_callback);
    view_dispatcher_add_view(app->dispatcher, ViewMenu, submenu_get_view(app->menu));

    app->status = view_alloc();
    view_set_context(app->status, app);
    view_allocate_model(app->status, ViewModelTypeLocking, sizeof(StatusModel));
    view_set_draw_callback(app->status, status_draw);
    view_set_input_callback(app->status, status_input);
    view_set_previous_callback(app->status, menu_previous_callback);
    view_dispatcher_add_view(app->dispatcher, ViewStatus, app->status);

    app->pick = submenu_alloc();
    view_set_previous_callback(submenu_get_view(app->pick), menu_previous_callback);
    view_dispatcher_add_view(app->dispatcher, ViewPick, submenu_get_view(app->pick));

    app->settings = variable_item_list_alloc();
    settings_build(app);
    view_set_previous_callback(variable_item_list_get_view(app->settings), menu_previous_callback);
    view_dispatcher_add_view(app->dispatcher, ViewSettings, variable_item_list_get_view(app->settings));

    app->about = widget_alloc();
    widget_add_text_scroll_element(
        app->about,
        0,
        0,
        128,
        64,
        "\e#Govee Party\n"
        "Finds Govee BLE lights\nand makes them dance.\n\n"
        "\e#Dance Party!\n"
        "Every light: on, 100%,\nmusic mode (mic) so it\nkeeps going after you leave.\n\n"
        "\e#Live DJ\n"
        "Stream patterns to one\nlight. Left/Right pattern,\nUp/Down speed, OK hold.\n\n"
        "\e#Govee B-Gone\n"
        "Every light: off. Turn on\n'lurks' to keep them off.\n\n"
        "\e#Radio\n"
        "Flipper BLE needs firmware\nwith BLE central (PR #4436)\n+ Full BLE stack.\n"
        "ESP32 bridge: flash\nesp32/govee_bridge, wire\n13>RX 14<TX 5V GND.\n\n"
        "Protocol from the\nGovee-Bluetooth HA\nintegration.");
    view_set_previous_callback(widget_get_view(app->about), menu_previous_callback);
    view_dispatcher_add_view(app->dispatcher, ViewAbout, widget_get_view(app->about));

    view_dispatcher_switch_to_view(app->dispatcher, ViewMenu);
    return app;
}

static void app_free(App* app) {
    worker_stop(app);
    flipper_led_off(app);
    view_dispatcher_remove_view(app->dispatcher, ViewMenu);
    view_dispatcher_remove_view(app->dispatcher, ViewStatus);
    view_dispatcher_remove_view(app->dispatcher, ViewPick);
    view_dispatcher_remove_view(app->dispatcher, ViewSettings);
    view_dispatcher_remove_view(app->dispatcher, ViewAbout);
    submenu_free(app->menu);
    submenu_free(app->pick);
    view_free(app->status);
    variable_item_list_free(app->settings);
    widget_free(app->about);
    view_dispatcher_free(app->dispatcher);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t govee_party_app(void* p) {
    UNUSED(p);
    App* app = app_alloc();
    view_dispatcher_run(app->dispatcher);
    app_free(app);
    return 0;
}
