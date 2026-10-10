// libpad on SDL 3: each port is the SDL gamepad that was connected first and
// is not on the other port, presented to the game as a DualShock 2 in analog
// mode. Any controller SDL knows (Xbox, PlayStation, Switch, generic) works,
// with its buttons mapped by position.

#include <SDL3/SDL.h>
#include <libpad.h>

#include <cstring>

namespace {

const int kPortCount = 2;

/** Terminal ID a DualShock 2 reports in analog mode. */
const int kTerminalDualShock = 7;

/** A trigger counts as L2 or R2 held from a quarter of its travel. */
const Sint16 kTriggerThreshold = 0x2000;

/** How long one scePadSetActDirect keeps the motors running; the game refreshes it each frame. */
const Uint32 kRumbleMs = 200;

/** Bits of the button word: high byte data[2], low byte data[3]. */
enum PadButton {
    PAD_BUTTON_L2 = 0x0001,
    PAD_BUTTON_R2 = 0x0002,
    PAD_BUTTON_L1 = 0x0004,
    PAD_BUTTON_R1 = 0x0008,
    PAD_BUTTON_TRIANGLE = 0x0010,
    PAD_BUTTON_CIRCLE = 0x0020,
    PAD_BUTTON_CROSS = 0x0040,
    PAD_BUTTON_SQUARE = 0x0080,
    PAD_BUTTON_SELECT = 0x0100,
    PAD_BUTTON_L3 = 0x0200,
    PAD_BUTTON_R3 = 0x0400,
    PAD_BUTTON_START = 0x0800,
    PAD_BUTTON_UP = 0x1000,
    PAD_BUTTON_RIGHT = 0x2000,
    PAD_BUTTON_DOWN = 0x4000,
    PAD_BUTTON_LEFT = 0x8000,
};

struct ButtonMapping {
    SDL_GamepadButton sdl;
    int               pad;
};

const ButtonMapping kButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH,          PAD_BUTTON_CROSS   },
    {SDL_GAMEPAD_BUTTON_EAST,           PAD_BUTTON_CIRCLE  },
    {SDL_GAMEPAD_BUTTON_WEST,           PAD_BUTTON_SQUARE  },
    {SDL_GAMEPAD_BUTTON_NORTH,          PAD_BUTTON_TRIANGLE},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  PAD_BUTTON_L1      },
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_BUTTON_R1      },
    {SDL_GAMEPAD_BUTTON_BACK,           PAD_BUTTON_SELECT  },
    {SDL_GAMEPAD_BUTTON_START,          PAD_BUTTON_START   },
    {SDL_GAMEPAD_BUTTON_LEFT_STICK,     PAD_BUTTON_L3      },
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK,    PAD_BUTTON_R3      },
    {SDL_GAMEPAD_BUTTON_DPAD_UP,        PAD_BUTTON_UP      },
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     PAD_BUTTON_RIGHT   },
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN,      PAD_BUTTON_DOWN    },
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT,      PAD_BUTTON_LEFT    },
};

struct Port {
    bool         open;
    SDL_Gamepad *gamepad;
};

Port ports[kPortCount];
bool initialised;

bool IsAssigned(SDL_JoystickID id) {
    for (int i = 0; i < kPortCount; i++) {
        if (ports[i].gamepad != NULL && SDL_GetGamepadID(ports[i].gamepad) == id) {
            return true;
        }
    }

    return false;
}

/** Drops a port's gamepad once it is unplugged and takes the first free one, if any. */
SDL_Gamepad *Refresh(int port) {
    if (!initialised || port < 0 || port >= kPortCount || !ports[port].open) {
        return NULL;
    }

    SDL_UpdateGamepads();
    Port &p = ports[port];

    if (p.gamepad != NULL && !SDL_GamepadConnected(p.gamepad)) {
        SDL_CloseGamepad(p.gamepad);
        p.gamepad = NULL;
    }

    if (p.gamepad == NULL) {
        int             count = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&count);

        for (int i = 0; ids != NULL && i < count; i++) {
            if (!IsAssigned(ids[i])) {
                p.gamepad = SDL_OpenGamepad(ids[i]);
                break;
            }
        }

        SDL_free(ids);
    }

    return p.gamepad;
}

/** Turns an SDL stick axis into the 0-255 range of a DualShock, 0x80 at rest. */
unsigned char StickByte(SDL_Gamepad *gamepad, SDL_GamepadAxis axis) {
    return (unsigned char) ((SDL_GetGamepadAxis(gamepad, axis) + 32768) >> 8);
}

} // namespace

extern "C" {

int scePadInit(int mode) {
    (void) mode;

    if (!initialised) {
        initialised = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    }

    return initialised ? 1 : 0;
}

int scePadEnd(void) {
    for (int i = 0; i < kPortCount; i++) {
        scePadPortClose(i, 0);
    }

    if (initialised) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        initialised = false;
    }

    return 1;
}

int scePadPortOpen(int port, int slot, unsigned char *buffer) {
    (void) slot;
    (void) buffer;

    if (!initialised || port < 0 || port >= kPortCount) {
        return 0;
    }

    ports[port].open = true;
    return 1;
}

int scePadPortClose(int port, int slot) {
    (void) slot;

    if (port < 0 || port >= kPortCount) {
        return 0;
    }

    if (ports[port].gamepad != NULL) {
        SDL_CloseGamepad(ports[port].gamepad);
        ports[port].gamepad = NULL;
    }

    ports[port].open = false;
    return 1;
}

int scePadGetState(int port, int slot) {
    (void) slot;
    return Refresh(port) != NULL ? scePadStateStable : scePadStateDiscon;
}

int scePadInfoMode(int port, int slot, int info, int index) {
    (void) slot;
    (void) index;

    if (Refresh(port) == NULL) {
        return 0;
    }

    // An analog DualShock 2 with no extended ID.
    return info == InfoModeCurID ? kTerminalDualShock : 0;
}

int scePadSetMainMode(int port, int slot, int mode, int lock) {
    (void) slot;
    (void) mode;
    (void) lock;
    return Refresh(port) != NULL ? 1 : 0;
}

int scePadInfoAct(int port, int slot, int actuator, int command) {
    (void) slot;
    (void) command;

    if (Refresh(port) == NULL) {
        return 0;
    }

    // -1 asks for the number of actuators: a small and a large motor.
    return actuator == -1 ? 2 : 1;
}

int scePadSetActAlign(int port, int slot, unsigned char *alignment) {
    (void) slot;
    (void) alignment;
    return Refresh(port) != NULL ? 1 : 0;
}

int scePadSetActDirect(int port, int slot, unsigned char *values) {
    (void) slot;
    SDL_Gamepad *gamepad = Refresh(port);

    if (gamepad == NULL || values == NULL) {
        return 0;
    }

    // values[0] switches the small motor on or off, values[1] drives the large
    // one from 0 to 255; SDL's low-frequency motor is the large one.
    Uint16 large = (Uint16) (values[1] * 257);
    Uint16 small = values[0] ? 0xFFFF : 0;
    SDL_RumbleGamepad(gamepad, large, small, large || small ? kRumbleMs : 0);
    return 1;
}

int scePadRead(int port, int slot, unsigned char *data) {
    (void) slot;
    SDL_Gamepad *gamepad = Refresh(port);

    if (gamepad == NULL) {
        return 0;
    }

    int buttons = 0;

    for (const ButtonMapping &mapping : kButtons) {
        if (SDL_GetGamepadButton(gamepad, mapping.sdl)) {
            buttons |= mapping.pad;
        }
    }

    if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >= kTriggerThreshold) {
        buttons |= PAD_BUTTON_L2;
    }

    if (SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >= kTriggerThreshold) {
        buttons |= PAD_BUTTON_R2;
    }

    // What libpad hands back for an analog controller: a success byte, the
    // terminal ID and data length in words, the buttons active low, then the
    // right and left sticks.
    std::memset(data, 0, 32);
    data[0] = 0;
    data[1] = (unsigned char) (kTerminalDualShock << 4 | 3);
    data[2] = (unsigned char) (~buttons >> 8);
    data[3] = (unsigned char) ~buttons;
    data[4] = StickByte(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
    data[5] = StickByte(gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
    data[6] = StickByte(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    data[7] = StickByte(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
    return 32;
}

} // extern "C"
