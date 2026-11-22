// Phones as controllers (phonepad.h): the server's thread and each phone's virtual SDL gamepad.
// The certificate and the code a phone must send are kept in <game folder>/phonepad.bin, so a phone that accepted the
// certificate, and a page left open, still work at the next start; a new certificate is made when the PC's address
// changed (the certificate names it) or the old one nears its end.
// Every SDL call here is on the server's thread, but for the gamepad's Rumble callback, which only stores the motors.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "ov.h"
#include "phonepad.h"
#include "phonepad_srv.h"
#include "platform/platform.h"
#include "ui/menu/menu.h"
#include "ui_text.h"

enum {
    NET_MS = 15000, NET_RETRY_MS = 60000,   // the PC's address is looked up again: it changes with the network
    // Nintendo's ids, as switch2.c's gamepads: SDL then labels the east button A, the layout the page draws
    VENDOR_NINTENDO = 0x057E, PRODUCT_PRO_CONTROLLER_2 = 0x2069,
};

typedef struct {
    SDL_JoystickID id;
    SDL_Joystick *js;
    _Atomic uint32_t rumble;   // low << 16 | high, as the game last set them (Rumble callback, any thread)
} Pad;

typedef struct {
    PhoneCert cert;
    char code[PPAD_CODE_LEN + 1];
} Saved;

static PlatMutex lock = PLAT_MUTEX_INIT;   // info and running
static PhonePadInfo info;
static int running;

void phonepad_info(PhonePadInfo *out) {
    plat_mutex_lock(&lock);
    *out = info;
    plat_mutex_unlock(&lock);
}

static void publish(const PhonePadInfo *now) {
    plat_mutex_lock(&lock);
    info = *now;
    plat_mutex_unlock(&lock);
}

// ---- a phone's gamepad

static bool SDLCALL pad_rumble(void *userdata, Uint16 low, Uint16 high) {
    atomic_store(&((Pad *)userdata)->rumble, (uint32_t)low << 16 | high);
    return true;
}

static void pad_state(void *ctx, void *pad, const PpadState *s) {
    (void)ctx;
    static const struct { int bit; SDL_GamepadButton sdl; } map[] = {
        {PPAD_A, SDL_GAMEPAD_BUTTON_EAST}, {PPAD_B, SDL_GAMEPAD_BUTTON_SOUTH}, {PPAD_X, SDL_GAMEPAD_BUTTON_NORTH},
        {PPAD_Y, SDL_GAMEPAD_BUTTON_WEST}, {PPAD_L, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {PPAD_R, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {PPAD_PLUS, SDL_GAMEPAD_BUTTON_START},
        {PPAD_MINUS, SDL_GAMEPAD_BUTTON_BACK}, {PPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_UP},
        {PPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_DOWN}, {PPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
        {PPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT}, {PPAD_LSTICK, SDL_GAMEPAD_BUTTON_LEFT_STICK},
        {PPAD_RSTICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    };
    SDL_Joystick *js = ((Pad *)pad)->js;
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) SDL_SetJoystickVirtualButton(js, map[i].sdl, (s->buttons >> map[i].bit) & 1);
    // a trigger at rest is the axis' minimum (SDL_joystick.h): at 0 it would read as half pressed
    SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, (s->buttons >> PPAD_ZL) & 1 ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
    SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, (s->buttons >> PPAD_ZR) & 1 ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
    // y up in a packet, down in SDL
    SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_LEFTX, s->stick[0]);
    SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_LEFTY, (Sint16)-s->stick[1]);
    SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_RIGHTX, s->stick[2]);
    SDL_SetJoystickVirtualAxis(js, SDL_GAMEPAD_AXIS_RIGHTY, (Sint16)-s->stick[3]);
    if (!s->motion) return;   // no sensors (the player refused them): the last sample stays, a controller at rest
    float acc[3], gyro[3];
    ppad_motion_sdl(s, acc, gyro);
    uint64_t at = SDL_GetTicksNS();
    SDL_SendJoystickVirtualSensorData(js, SDL_SENSOR_ACCEL, at, acc, 3);
    SDL_SendJoystickVirtualSensorData(js, SDL_SENSOR_GYRO, at, gyro, 3);
}

static void *pad_attach(void *ctx, const char *peer) {
    (void)ctx;
    static const SDL_VirtualJoystickSensorDesc sensors[2] = {{SDL_SENSOR_ACCEL, 60.f}, {SDL_SENSOR_GYRO, 60.f}};
    Pad *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.vendor_id = VENDOR_NINTENDO;
    d.product_id = PRODUCT_PRO_CONTROLLER_2;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.nsensors = 2;
    d.sensors = sensors;
    d.name = "Phone controller";
    d.userdata = p;
    d.Rumble = pad_rumble;
    p->id = SDL_AttachVirtualJoystick(&d);
    p->js = p->id ? SDL_OpenJoystick(p->id) : NULL;
    if (!p->js) {
        rt_log("input: phone %s: no virtual gamepad (%s)", peer, SDL_GetError());
        if (p->id) SDL_DetachVirtualJoystick(p->id);
        free(p);
        return NULL;
    }
    // released, centred and lying still until its first packet
    PpadState rest = {0};
    float acc[3], gyro[3];
    pad_state(NULL, p, &rest);
    ppad_motion_rest(acc, gyro);
    SDL_SendJoystickVirtualSensorData(p->js, SDL_SENSOR_ACCEL, SDL_GetTicksNS(), acc, 3);
    SDL_SendJoystickVirtualSensorData(p->js, SDL_SENSOR_GYRO, SDL_GetTicksNS(), gyro, 3);
    menu_notice(menu_text(UI_NOTICE_PHONE_JOINED), 4);
    return p;
}

static void pad_detach(void *ctx, void *pad) {
    (void)ctx;
    Pad *p = pad;
    SDL_CloseJoystick(p->js);
    SDL_DetachVirtualJoystick(p->id);
    free(p);
}

static void pad_feedback(void *ctx, void *pad, uint8_t out[PPAD_REPLY_LEN]) {
    (void)ctx;
    Pad *p = pad;
    uint32_t v = atomic_load(&p->rumble);
    out[0] = (uint8_t)(v >> 24), out[1] = (uint8_t)(v >> 8);   // the motors' high bytes
    out[2] = (uint8_t)(input_pad_player(p->id) + 1);            // 0 until input.c gave it to a player
}

static void pad_log(void *ctx, const char *line) {
    (void)ctx;
    rt_log("%s", line);
}

// ---- the server's thread

// The certificate and the code of the last start when they still serve (the same address, not near its end), else
// new ones, saved. 0 when the system gives no random numbers.
static int saved_load(const uint8_t ip[4], Saved *out) {
    char path[PLAT_PATH_MAX * 2];
    snprintf(path, sizeof path, "%s/phonepad.bin", cfg_game_dir());
    int64_t now = (int64_t)(plat_unix_time_ns() / 1000000000ull);
    uint64_t size = 0;
    uint8_t *file = plat_file_read(path, &size);
    int code_ok = file && size == sizeof *out;
    if (code_ok) {
        memcpy(out, file, sizeof *out);
        code_ok = strlen(out->code) == PPAD_CODE_LEN && strspn(out->code, "0123456789") == PPAD_CODE_LEN;
    }
    free(file);
    if (code_ok && phonecert_usable(&out->cert, sizeof out->cert, ip, now)) return 1;
    if (!code_ok) {   // the code outlives the certificate: a page left open on a phone still joins
        uint8_t r[PPAD_CODE_LEN];
        if (!plat_random(r, sizeof r)) return 0;
        for (int i = 0; i < PPAD_CODE_LEN; i++) out->code[i] = (char)('0' + r[i] % 10);
        out->code[PPAD_CODE_LEN] = 0;
    }
    if (!phonecert_make(ip, now, &out->cert)) return 0;
    if (!plat_file_write_atomic(path, out, sizeof *out))
        rt_log("input: phones: %s was not written: they will be asked to accept a new certificate at the next start", path);
    return 1;
}

static void phone_main(void *arg) {
    (void)arg;
    static Saved saved;   // the server keeps pointers into it
    const PhoneSink sink = {NULL, pad_attach, pad_detach, pad_state, pad_feedback, pad_log};
    PhoneSrv *srv = NULL;
    PhonePadInfo now = {0};
    uint8_t ip[4] = {0};
    uint64_t next_net = 0;
    int port = cfg_int("MKW_PHONE_PORT");
    if (port < 1024 || port > 65535) port = 6590;
    while (cfg_bool("MKW_PHONE_PADS")) {
        if (plat_now_ms() >= next_net) {
            uint8_t cur[4], mask[4];
            int have = !ov_lan_address(cur, mask);
            if (have && (!srv || memcmp(cur, ip, 4))) {   // the first look, or the PC moved to another network
                if (srv) phonesrv_close(srv);
                srv = NULL;
                memcpy(ip, cur, 4);
                PhoneSrvConfig cfg = {.port = (uint16_t)port, .cert = &saved.cert,
                                      .verbose = !strcmp(cfg_value("MKW_PHONE_PADS"), "2")};
                if (saved_load(ip, &saved)) {
                    memcpy(cfg.code, saved.code, sizeof cfg.code);
                    srv = phonesrv_open(&cfg, &sink);
                }
                now = (PhonePadInfo){.on = srv != NULL, .failed = srv == NULL, .port = port};
                if (srv) {
                    snprintf(now.address, sizeof now.address, "https://%d.%d.%d.%d:%d", ip[0], ip[1], ip[2], ip[3], port);
                    snprintf(now.code, sizeof now.code, "%s", saved.code);
                    snprintf(now.url, sizeof now.url, "%s/#%s", now.address, saved.code);
                    rt_log("input: phones as controllers at %s, code %s (MKW_PHONE_PADS)", now.address, now.code);
                }
                publish(&now);
            } else if (!have && !srv && !now.failed) {
                now = (PhonePadInfo){.failed = 1, .port = port};
                publish(&now);
                rt_log("input: phones as controllers: this PC has no address on a local network; looking again every minute");
            }
            next_net = plat_now_ms() + (srv ? NET_MS : NET_RETRY_MS);   // a taken port is tried again every minute too
        }
        if (!srv) {
            plat_sleep_ms(250);
            continue;
        }
        phonesrv_poll(srv, 10);
        if (phonesrv_pads(srv) != now.pads) {
            now.pads = phonesrv_pads(srv);
            publish(&now);
        }
    }
    if (srv) phonesrv_close(srv);
    rt_log("input: phones as controllers off");
    plat_mutex_lock(&lock);
    info = (PhonePadInfo){0};
    running = 0;
    plat_mutex_unlock(&lock);
}

void phonepad_start(void) {
    if (!cfg_bool("MKW_PHONE_PADS")) return;
    plat_mutex_lock(&lock);
    int start = !running;
    running = 1;
    plat_mutex_unlock(&lock);
    if (!start || plat_thread_spawn(phone_main, NULL, "phonepad", 0)) return;
    static int said;   // input.c asks again every second
    if (!said++) rt_log("input: phones as controllers: cannot start their thread");
    plat_mutex_lock(&lock);
    running = 0;
    plat_mutex_unlock(&lock);
}
