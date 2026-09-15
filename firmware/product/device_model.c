#include "device_model.h"

/* Slot order is the catalog presentation order (slot 0..key_count-1). */
static const rbp_key_def_t rc003_keys[] = {
    { RBP_KEY_POWER, "Power" },
    { RBP_KEY_UP, "Up" },
    { RBP_KEY_DOWN, "Down" },
    { RBP_KEY_LEFT, "Left" },
    { RBP_KEY_RIGHT, "Right" },
    { RBP_KEY_OK, "OK" },
    { RBP_KEY_BACK, "Back" },
    { RBP_KEY_HOME, "Home" },
    { RBP_KEY_MENU, "Menu" },
    { RBP_KEY_TV, "TV" },
    { RBP_KEY_VOLUME_UP, "Volume Up" },
    { RBP_KEY_VOLUME_DOWN, "Volume Down" },
    { RBP_KEY_VOICE, "Voice" },
};

const rbp_device_profile_t RBP_PROFILE_RC003 = {
    "xiaomi.rc003",
    "Xiaomi Remote 2 Pro",
    1,
    sizeof(rc003_keys) / sizeof(rc003_keys[0]) - 1,
    rc003_keys,
};

const rbp_device_profile_t RBP_PROFILE_RC003_VOICE = {"xiaomi.rc003", "Xiaomi Remote 2 Pro", 2, sizeof(rc003_keys)/sizeof(rc003_keys[0]), rc003_keys};
