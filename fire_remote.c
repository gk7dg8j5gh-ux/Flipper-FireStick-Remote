#include <furi.h>
#include <furi_hal_bt.h>
#include <extra_profiles/hid_profile.h>
#include <bt/bt_service/bt.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>

#define FIRE_REMOTE_BT_KEYS ".bt_fire_remote.keys"

#define FR_KEY_ENTER  0x28
#define FR_KEY_ESCAPE 0x29
#define FR_KEY_RIGHT  0x4F
#define FR_KEY_LEFT   0x50
#define FR_KEY_DOWN   0x51
#define FR_KEY_UP     0x52

#define FR_CONSUMER_MENU         0x0040
#define FR_CONSUMER_FAST_FORWARD 0x00B3
#define FR_CONSUMER_REWIND       0x00B4
#define FR_CONSUMER_PLAY_PAUSE   0x00CD
#define FR_CONSUMER_AC_HOME      0x0223

typedef struct {
    Bt* bt;
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    FuriHalBleProfileBase* hid_profile;
    bool connected;
    bool running;
} FireRemoteApp;

static void fire_remote_draw_callback(Canvas* canvas, void* context) {
    FireRemoteApp* app = context;
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Fire Remote");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 88, 10, app->connected ? "BT:OK" : "PAIR");

    canvas_draw_str(canvas, 2, 21,
        app->connected ? "D-pad nav  OK=select" : "Fire TV: Add BT device");

    canvas_draw_str(canvas, 61, 32, "^");
    canvas_draw_str(canvas, 44, 42, "<");
    canvas_draw_str(canvas, 58, 42, "OK");
    canvas_draw_str(canvas, 82, 42, ">");
    canvas_draw_str(canvas, 61, 50, "v");

    canvas_draw_str(canvas, 2, 53, "Hold: ^Home vMenu");
    canvas_draw_str(canvas, 2, 63, "<RW >FF OK=Play B=Exit");
}

static void fire_remote_input_callback(InputEvent* event, void* context) {
    FireRemoteApp* app = context;
    furi_message_queue_put(app->input_queue, event, 0);
}

static void fire_remote_bt_status_callback(BtStatus status, void* context) {
    FireRemoteApp* app = context;
    app->connected = (status == BtStatusConnected);
    if(app->view_port) view_port_update(app->view_port);
}

static void fire_remote_tap_keyboard(FireRemoteApp* app, uint16_t key) {
    if(!app->hid_profile) return;
    ble_profile_hid_kb_press(app->hid_profile, key);
    furi_delay_ms(20);
    ble_profile_hid_kb_release(app->hid_profile, key);
}

static void fire_remote_tap_consumer(FireRemoteApp* app, uint16_t key) {
    if(!app->hid_profile) return;
    ble_profile_hid_consumer_key_press(app->hid_profile, key);
    furi_delay_ms(20);
    ble_profile_hid_consumer_key_release(app->hid_profile, key);
}

static void fire_remote_handle_short(FireRemoteApp* app, InputKey key) {
    switch(key) {
    case InputKeyUp:    fire_remote_tap_keyboard(app, FR_KEY_UP); break;
    case InputKeyDown:  fire_remote_tap_keyboard(app, FR_KEY_DOWN); break;
    case InputKeyLeft:  fire_remote_tap_keyboard(app, FR_KEY_LEFT); break;
    case InputKeyRight: fire_remote_tap_keyboard(app, FR_KEY_RIGHT); break;
    case InputKeyOk:    fire_remote_tap_keyboard(app, FR_KEY_ENTER); break;
    case InputKeyBack:  fire_remote_tap_keyboard(app, FR_KEY_ESCAPE); break;
    default: break;
    }
}

static void fire_remote_handle_long(FireRemoteApp* app, InputKey key) {
    switch(key) {
    case InputKeyUp:    fire_remote_tap_consumer(app, FR_CONSUMER_AC_HOME); break;
    case InputKeyDown:  fire_remote_tap_consumer(app, FR_CONSUMER_MENU); break;
    case InputKeyLeft:  fire_remote_tap_consumer(app, FR_CONSUMER_REWIND); break;
    case InputKeyRight: fire_remote_tap_consumer(app, FR_CONSUMER_FAST_FORWARD); break;
    case InputKeyOk:    fire_remote_tap_consumer(app, FR_CONSUMER_PLAY_PAUSE); break;
    case InputKeyBack:  app->running = false; break;
    default: break;
    }
}

int32_t fire_remote_app(void* p) {
    UNUSED(p);

    FireRemoteApp* app = malloc(sizeof(FireRemoteApp));
    memset(app, 0, sizeof(FireRemoteApp));
    app->running = true;

    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->gui = furi_record_open(RECORD_GUI);
    app->bt = furi_record_open(RECORD_BT);

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, fire_remote_draw_callback, app);
    view_port_input_callback_set(app->view_port, fire_remote_input_callback, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    bt_disconnect(app->bt);
    furi_delay_ms(200);
    bt_keys_storage_set_storage_path(app->bt, "/ext/apps_data/fire_remote/.bt_fire_remote.keys");

    static const BleProfileHidParams hid_params = {
        .device_name_prefix = "Fire",
        .mac_xor = 0xF17E,
    };

    app->hid_profile = bt_profile_start(app->bt, ble_profile_hid, (void*)&hid_params);
    furi_check(app->hid_profile);

    bt_set_status_changed_callback(app->bt, fire_remote_bt_status_callback, app);
    furi_hal_bt_start_advertising();
    view_port_update(app->view_port);

    InputEvent event;
    while(app->running) {
        if(furi_message_queue_get(app->input_queue, &event, 100) == FuriStatusOk) {
            if(event.type == InputTypeShort) {
                fire_remote_handle_short(app, event.key);
            } else if(event.type == InputTypeLong) {
                fire_remote_handle_long(app, event.key);
            }
        }
    }

    ble_profile_hid_kb_release_all(app->hid_profile);
    ble_profile_hid_consumer_key_release_all(app->hid_profile);

    bt_set_status_changed_callback(app->bt, NULL, NULL);
    bt_disconnect(app->bt);
    furi_delay_ms(200);
    bt_keys_storage_set_default_path(app->bt);
    furi_check(bt_profile_restore_default(app->bt));

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_message_queue_free(app->input_queue);

    furi_record_close(RECORD_BT);
    furi_record_close(RECORD_GUI);
    free(app);
    return 0;
}
