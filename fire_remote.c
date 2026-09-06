#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_bt.h>
#include <furi_ble/profile_interface.h>
#include <ble/ble.h>
#include <services/dev_info_service.h>
#include <services/battery_service.h>
#include <extra_services/hid_service.h>
#include <bt/bt_service/bt.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>

/*
 * FirePad: BLE HID gamepad experiment for Fire TV.
 * This does NOT use Flipper's stock keyboard/mouse HID descriptor.
 * It installs a custom Game Pad HID report map and advertises with
 * the Bluetooth HID Gamepad appearance (0x03C4).
 */

#define FIREPAD_KEYS_PATH "/ext/apps_data/fire_remote/.bt_firepad.keys"

#define FIREPAD_HID_INFO_USB_SPEC 0x0101
#define FIREPAD_HID_COUNTRY_CODE  0x00
#define FIREPAD_HID_FLAGS         0x03

#define FIREPAD_REPORT_NUM 0
#define FIREPAD_APPEARANCE_GAMEPAD 0x03C4

typedef struct {
    const char* device_name_prefix;
    uint16_t mac_xor;
} FirePadProfileParams;

typedef struct {
    uint8_t buttons;   // bits 0..7 = buttons 1..8
    uint8_t hat;       // low nibble = hat 0..7, 8 = neutral
    int8_t x;
    int8_t y;
} FURI_PACKED FirePadReport;

typedef struct {
    FuriHalBleProfileBase base;
    BleServiceBattery* battery_svc;
    BleServiceDevInfo* dev_info_svc;
    BleServiceHid* hid_svc;
    FirePadReport report;
} FirePadProfile;

static const FuriHalBleProfileTemplate* firepad_profile_template_ptr;

/*
 * Report ID 1:
 *  - 8 buttons
 *  - one 8-way hat switch with null state
 *  - X/Y axes (left neutral; included for broad gamepad compatibility)
 */
static const uint8_t firepad_report_map[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x01,       //   Report ID (1)

    0x05, 0x09,       //   Usage Page (Button)
    0x19, 0x01,       //   Usage Minimum (Button 1)
    0x29, 0x08,       //   Usage Maximum (Button 8)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x95, 0x08,       //   Report Count (8)
    0x75, 0x01,       //   Report Size (1)
    0x81, 0x02,       //   Input (Data,Var,Abs)

    0x05, 0x01,       //   Usage Page (Generic Desktop)
    0x09, 0x39,       //   Usage (Hat Switch)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x07,       //   Logical Maximum (7)
    0x35, 0x00,       //   Physical Minimum (0)
    0x46, 0x3B, 0x01, //   Physical Maximum (315)
    0x65, 0x14,       //   Unit (degrees)
    0x75, 0x04,       //   Report Size (4)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x42,       //   Input (Data,Var,Abs,Null)
    0x65, 0x00,       //   Unit (None)
    0x75, 0x04,       //   Report Size (4)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x03,       //   Input (Const,Var,Abs)

    0x09, 0x30,       //   Usage (X)
    0x09, 0x31,       //   Usage (Y)
    0x15, 0x81,       //   Logical Minimum (-127)
    0x25, 0x7F,       //   Logical Maximum (127)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x02,       //   Report Count (2)
    0x81, 0x02,       //   Input (Data,Var,Abs)

    0xC0              // End Collection
};

static FuriHalBleProfileBase* firepad_profile_start(FuriHalBleProfileParams params) {
    UNUSED(params);

    FirePadProfile* profile = malloc(sizeof(FirePadProfile));
    memset(profile, 0, sizeof(FirePadProfile));

    profile->base.config = firepad_profile_template_ptr;
    profile->report.hat = 8; // null/centered

    profile->battery_svc = ble_svc_battery_start(true);
    profile->dev_info_svc = ble_svc_dev_info_start();
    profile->hid_svc = ble_svc_hid_start();

    furi_check(profile->battery_svc);
    furi_check(profile->dev_info_svc);
    furi_check(profile->hid_svc);

    furi_check(ble_svc_hid_update_report_map(
        profile->hid_svc, firepad_report_map, sizeof(firepad_report_map)));

    uint8_t hid_info[4] = {
        FIREPAD_HID_INFO_USB_SPEC & 0xFF,
        (FIREPAD_HID_INFO_USB_SPEC >> 8) & 0xFF,
        FIREPAD_HID_COUNTRY_CODE,
        FIREPAD_HID_FLAGS,
    };
    furi_check(ble_svc_hid_update_info(profile->hid_svc, hid_info));

    return &profile->base;
}

static void firepad_profile_stop(FuriHalBleProfileBase* base) {
    furi_check(base);
    furi_check(base->config == firepad_profile_template_ptr);

    FirePadProfile* profile = (FirePadProfile*)base;
    ble_svc_battery_stop(profile->battery_svc);
    ble_svc_dev_info_stop(profile->dev_info_svc);
    ble_svc_hid_stop(profile->hid_svc);
    free(profile);
}

#define CONNECTION_INTERVAL_MIN 0x0006
#define CONNECTION_INTERVAL_MAX 0x0024

static GapConfig firepad_gap_template = {
    .adv_service = {
        .UUID_Type = UUID_TYPE_16,
        .Service_UUID_16 = HUMAN_INTERFACE_DEVICE_SERVICE_UUID,
    },
    .appearance_char = FIREPAD_APPEARANCE_GAMEPAD,
    .bonding_mode = true,
    .pairing_method = GapPairingPinCodeVerifyYesNo,
    .conn_param = {
        .conn_int_min = CONNECTION_INTERVAL_MIN,
        .conn_int_max = CONNECTION_INTERVAL_MAX,
        .slave_latency = 0,
        .supervisor_timeout = 0,
    },
};

static void firepad_get_gap_config(GapConfig* config, FuriHalBleProfileParams raw_params) {
    FirePadProfileParams* params = raw_params;
    furi_check(config);
    memcpy(config, &firepad_gap_template, sizeof(GapConfig));

    memcpy(config->mac_address, furi_hal_version_get_ble_mac(), sizeof(config->mac_address));
    config->mac_address[2]++;

    if(params) {
        config->mac_address[0] ^= params->mac_xor & 0xFF;
        config->mac_address[1] ^= (params->mac_xor >> 8) & 0xFF;
    }

    memset(config->adv_name, 0, sizeof(config->adv_name));
    FuriString* name = furi_string_alloc_set(furi_hal_version_get_ble_local_device_name_ptr());
    const char* prefix = (params && params->device_name_prefix) ?
        params->device_name_prefix : "FirePad";
    furi_string_replace_str(name, "Flipper", prefix);
    if(furi_string_size(name) >= sizeof(config->adv_name)) {
        furi_string_left(name, sizeof(config->adv_name) - 1);
    }
    memcpy(config->adv_name, furi_string_get_cstr(name), furi_string_size(name));
    furi_string_free(name);
}

static const FuriHalBleProfileTemplate firepad_profile_template = {
    .start = firepad_profile_start,
    .stop = firepad_profile_stop,
    .get_gap_config = firepad_get_gap_config,
};

static const FuriHalBleProfileTemplate* firepad_profile_template_ptr =
    &firepad_profile_template;

static bool firepad_send(FuriHalBleProfileBase* base, const FirePadReport* report) {
    furi_check(base);
    furi_check(base->config == firepad_profile_template_ptr);
    FirePadProfile* profile = (FirePadProfile*)base;
    profile->report = *report;
    return ble_svc_hid_update_input_report(
        profile->hid_svc,
        FIREPAD_REPORT_NUM,
        (uint8_t*)&profile->report,
        sizeof(profile->report));
}

typedef struct {
    Bt* bt;
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    FuriHalBleProfileBase* profile;
    bool connected;
    bool running;
} FireRemoteApp;

static void fire_remote_draw(Canvas* canvas, void* context) {
    FireRemoteApp* app = context;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "FirePad");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 88, 10, app->connected ? "BT:OK" : "PAIR");

    canvas_draw_str(
        canvas, 2, 22,
        app->connected ? "Gamepad connected" : "Fire TV: Add controller");

    canvas_draw_str(canvas, 61, 34, "^");
    canvas_draw_str(canvas, 43, 44, "<");
    canvas_draw_str(canvas, 58, 44, "A");
    canvas_draw_str(canvas, 82, 44, ">");
    canvas_draw_str(canvas, 61, 52, "v");

    canvas_draw_str(canvas, 2, 63, "OK=A  Back=B/hold exit");
}

static void fire_remote_input(InputEvent* event, void* context) {
    FireRemoteApp* app = context;
    furi_message_queue_put(app->input_queue, event, 0);
}

static void fire_remote_bt_status(BtStatus status, void* context) {
    FireRemoteApp* app = context;
    app->connected = (status == BtStatusConnected);
    if(app->view_port) view_port_update(app->view_port);
}

static void firepad_tap_hat(FireRemoteApp* app, uint8_t hat) {
    if(!app->profile) return;
    FirePadReport r = {.buttons = 0, .hat = hat, .x = 0, .y = 0};
    firepad_send(app->profile, &r);
    furi_delay_ms(60);
    r.hat = 8;
    firepad_send(app->profile, &r);
}

static void firepad_tap_button(FireRemoteApp* app, uint8_t button_bit) {
    if(!app->profile) return;
    FirePadReport r = {.buttons = button_bit, .hat = 8, .x = 0, .y = 0};
    firepad_send(app->profile, &r);
    furi_delay_ms(60);
    r.buttons = 0;
    firepad_send(app->profile, &r);
}

static void fire_remote_short(FireRemoteApp* app, InputKey key) {
    switch(key) {
    case InputKeyUp:    firepad_tap_hat(app, 0); break;
    case InputKeyRight: firepad_tap_hat(app, 2); break;
    case InputKeyDown:  firepad_tap_hat(app, 4); break;
    case InputKeyLeft:  firepad_tap_hat(app, 6); break;
    case InputKeyOk:    firepad_tap_button(app, 1u << 0); break; // A
    case InputKeyBack:  firepad_tap_button(app, 1u << 1); break; // B
    default: break;
    }
}

static void fire_remote_long(FireRemoteApp* app, InputKey key) {
    switch(key) {
    case InputKeyLeft:  firepad_tap_button(app, 1u << 4); break; // LB
    case InputKeyRight: firepad_tap_button(app, 1u << 5); break; // RB
    case InputKeyUp:    firepad_tap_button(app, 1u << 2); break; // X
    case InputKeyDown:  firepad_tap_button(app, 1u << 3); break; // Y
    case InputKeyOk:    firepad_tap_button(app, 1u << 7); break; // Start/Menu-ish
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
    view_port_draw_callback_set(app->view_port, fire_remote_draw, app);
    view_port_input_callback_set(app->view_port, fire_remote_input, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    bt_disconnect(app->bt);
    furi_delay_ms(200);
    bt_keys_storage_set_storage_path(app->bt, FIREPAD_KEYS_PATH);

    static const FirePadProfileParams params = {
        .device_name_prefix = "FirePad",
        .mac_xor = 0xA55A,
    };

    app->profile = bt_profile_start(
        app->bt, firepad_profile_template_ptr, (void*)&params);
    furi_check(app->profile);

    bt_set_status_changed_callback(app->bt, fire_remote_bt_status, app);
    furi_hal_bt_start_advertising();
    view_port_update(app->view_port);

    InputEvent event;
    while(app->running) {
        if(furi_message_queue_get(app->input_queue, &event, 100) == FuriStatusOk) {
            if(event.type == InputTypeShort) {
                fire_remote_short(app, event.key);
            } else if(event.type == InputTypeLong) {
                fire_remote_long(app, event.key);
            }
        }
    }

    FirePadReport neutral = {.buttons = 0, .hat = 8, .x = 0, .y = 0};
    firepad_send(app->profile, &neutral);

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
