#include "config.h"
#include "util.h"
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <toml.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>

struct mapping {
    const char *name;
    int val;
};

static const struct mapping actions[] = {
    {"wall_reset_all", ACTION_WALL_RESET_ALL},
    {"wall_reset", ACTION_WALL_RESET_ONE},
    {"wall_play", ACTION_WALL_PLAY},
    {"wall_lock", ACTION_WALL_LOCK},
    {"wall_focus_reset", ACTION_WALL_FOCUS_RESET},
    {"ingame_reset", ACTION_INGAME_RESET},
    {"alt_res", ACTION_INGAME_ALT_RES},
    {"toggle_ninb", ACTION_ANY_TOGGLE_NINB},
};

static const struct mapping buttons[] = {
    {"lmb", BTN_LEFT},   {"mouse1", BTN_LEFT},   {"leftmouse", BTN_LEFT},
    {"rmb", BTN_RIGHT},  {"mouse2", BTN_RIGHT},  {"rightmouse", BTN_RIGHT},
    {"mmb", BTN_MIDDLE}, {"mouse3", BTN_MIDDLE}, {"middlemouse", BTN_MIDDLE},
};

static const struct mapping modifiers[] = {
    {"shift", WLR_MODIFIER_SHIFT},   {"caps", WLR_MODIFIER_CAPS},    {"lock", WLR_MODIFIER_CAPS},
    {"capslock", WLR_MODIFIER_CAPS}, {"control", WLR_MODIFIER_CTRL}, {"ctrl", WLR_MODIFIER_CTRL},
    {"alt", WLR_MODIFIER_ALT},       {"mod1", WLR_MODIFIER_ALT},     {"mod2", WLR_MODIFIER_MOD2},
    {"mod3", WLR_MODIFIER_MOD3},     {"super", WLR_MODIFIER_LOGO},   {"mod4", WLR_MODIFIER_LOGO},
    {"mod5", WLR_MODIFIER_MOD5},
};

const char config_filename[] = "waywall.toml";
static const char xdg_config_dir[] = "/.config";

static bool
parse_bind_array(toml_array_t *array, struct keybind *keybind, const char *key) {
    if (toml_array_nelem(array) > MAX_ACTIONS) {
        wlr_log(WLR_ERROR, "config: too many actions assigned to keybind '%s'", key);
        return false;
    } else if (toml_array_nelem(array) == 0) {
        wlr_log(WLR_ERROR, "config: no actions assigned to keybind '%s'", key);
        return false;
    }
    for (int j = 0; j < toml_array_nelem(array); j++) {
        toml_datum_t action = toml_string_at(array, j);
        if (!action.ok) {
            wlr_log(WLR_ERROR, "config: found non-string value for action %d of keybind '%s'", j,
                    key);
            free(action.u.s);
            return false;
        }
        bool found_action = false;
        for (unsigned long k = 0; k < ARRAY_LEN(actions); k++) {
            if (strcasecmp(action.u.s, actions[k].name) == 0) {
                keybind->actions[keybind->action_count++] = actions[k].val;
                found_action = true;
                break;
            }
        }
        if (!found_action) {
            wlr_log(WLR_ERROR, "config: unknown action '%s' assigned to keybind '%s'", action.u.s,
                    key);
            free(action.u.s);
            return false;
        }
        free(action.u.s);
    }
    return true;
}

static bool
parse_bind_table(toml_table_t *table, struct keybind *keybind, const char *key) {
    toml_array_t *actions = toml_array_in(table, "actions");
    if (!actions) {
        wlr_log(WLR_ERROR, "config: keybind '%s' has no 'actions' array", key);
        return false;
    }
    if (!parse_bind_array(actions, keybind, key)) {
        return false;
    }
    toml_datum_t allow_in_menu = toml_bool_in(table, "allow_in_menu");
    if (allow_in_menu.ok) {
        keybind->allow_in_menu = allow_in_menu.u.b;
    }
    toml_datum_t allow_in_pause = toml_bool_in(table, "allow_in_pause");
    if (allow_in_pause.ok) {
        keybind->allow_in_pause = allow_in_pause.u.b;
    }
    return true;
}

char *
config_get_dir() {
    const char *dir;
    if ((dir = getenv("XDG_CONFIG_HOME"))) {
        return strdup(dir);
    }
    if ((dir = getenv("HOME"))) {
        char *confdir = malloc(strlen(dir) + strlen(xdg_config_dir));
        memcpy(confdir, dir, strlen(dir) + 1);
        strcat(confdir, xdg_config_dir);
        return confdir;
    }
    wlr_log(WLR_ERROR, "could not find config directory");
    return NULL;
}

char *
config_get_path() {
    char *path = NULL;
    const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    if (xdg_config_home) {
        size_t len = strlen(xdg_config_home);
        path = malloc(len + strlen(config_filename) + 2);
        ww_assert(path);
        strcpy(path, xdg_config_home);
        strcat(path, "/");
        strcat(path, config_filename);
        return path;
    }

    const char *home = getenv("HOME");
    if (home) {
        size_t len = strlen(home);
        path = malloc(len + strlen(xdg_config_dir) + strlen(config_filename) + 2);
        ww_assert(path);
        strcpy(path, home);
        strcat(path, xdg_config_dir);
        strcat(path, "/");
        strcat(path, config_filename);
        return path;
    }

    wlr_log(WLR_ERROR, "no suitable directory found for config file");
    return NULL;
}

static bool
parse_bool(bool *value, toml_table_t *table, const char *value_name, const char *full_name,
           bool warn) {
    toml_datum_t datum = toml_bool_in(table, value_name);
    if (!datum.ok) {
        if (warn) {
            wlr_log(WLR_ERROR, "config: missing boolean value '%s'", full_name);
        }
        return false;
    }
    *value = datum.u.b;
    return true;
}

static bool
parse_color(float value[4], toml_table_t *table, const char *value_name, const char *full_name,
            bool warn) {
    toml_datum_t datum = toml_string_in(table, value_name);
    if (!datum.ok) {
        if (warn) {
            wlr_log(WLR_ERROR, "config: missing string value '%s'", full_name);
        }
        return false;
    }
    char *color = datum.u.s;
    size_t len = strlen(color);
    bool maybe_valid_rgb = len == 6 || (len == 7 && color[0] == '#');
    bool maybe_valid_rgba = len == 8 || (len == 9 && color[0] == '#');
    if (!maybe_valid_rgb && !maybe_valid_rgba) {
        wlr_log(WLR_ERROR, "config: invalid value ('%s') for color value '%s'", color, full_name);
        free(color);
        return false;
    }
    int r, g, b, a;
    if (maybe_valid_rgb) {
        int n = sscanf(color[0] == '#' ? color + 1 : color, "%02x%02x%02x", &r, &g, &b);
        if (n != 3) {
            wlr_log(WLR_ERROR, "config: invalid value ('%s') for color value '%s'", color,
                    full_name);
            free(color);
            return false;
        }
        value[0] = r / 255.0;
        value[1] = g / 255.0;
        value[2] = b / 255.0;
        value[3] = 1.0;
    } else {
        int n = sscanf(color[0] == '#' ? color + 1 : color, "%02x%02x%02x%02x", &r, &g, &b, &a);
        if (n != 4) {
            wlr_log(WLR_ERROR, "config: invalud value ('%s') for color value '%s'", color,
                    full_name);
            free(color);
            return false;
        }
        value[0] = r / 255.0;
        value[1] = g / 255.0;
        value[2] = b / 255.0;
        value[3] = a / 255.0;
    }
    free(color);
    return true;
}

static bool
parse_double(double *value, toml_table_t *table, const char *value_name, const char *full_name,
             bool warn) {
    toml_datum_t datum = toml_double_in(table, value_name);
    if (!datum.ok) {
        if (warn) {
            wlr_log(WLR_ERROR, "config: missing double value '%s'", full_name);
        }
        return false;
    }
    *value = datum.u.d;
    return true;
}

static bool
parse_int(int *value, toml_table_t *table, const char *value_name, const char *full_name,
          bool warn) {
    toml_datum_t datum = toml_int_in(table, value_name);
    if (!datum.ok) {
        if (warn) {
            wlr_log(WLR_ERROR, "config: missing integer value '%s'", full_name);
        }
        return false;
    }
    *value = datum.u.i;
    return true;
}

static bool
parse_str(char **value, toml_table_t *table, const char *value_name, const char *full_name,
          bool warn) {
    toml_datum_t datum = toml_string_in(table, value_name);
    if (!datum.ok) {
        if (warn) {
            wlr_log(WLR_ERROR, "config: missing string value '%s'", full_name);
        }
        return false;
    }
    *value = datum.u.s;
    return true;
}

static int
parse_enum(bool *ok, toml_table_t *table, const char *value_name, const char *full_name,
           const struct mapping *mappings, size_t mappings_count, bool warn) {
    char *str;
    if (!parse_str(&str, table, value_name, full_name, warn)) {
        *ok = false;
        return 0;
    }
    for (size_t i = 0; i < mappings_count; i++) {
        if (strcmp(mappings[i].name, str) == 0) {
            *ok = true;
            free(str);
            return mappings[i].val;
        }
    }
    *ok = false;

    // Generate the error message.
    char buf[1024], *ptr = buf;
    for (size_t i = 0; i < mappings_count; i++) {
        ww_assert((size_t)(ptr - buf) < ARRAY_LEN(buf));
        ptr += snprintf(ptr, 1024 - (ptr - buf), "'%s'%s", mappings[i].name,
                        (mappings_count - i) == 1 ? "" : ", ");
    }
    wlr_log(WLR_ERROR, "config: invalid enum value '%s' for '%s' (use one of: %s)", str, full_name,
            buf);
    free(str);
    return 0;
}

#define PARSE_BOOL(table, name)                                                                    \
    do {                                                                                           \
        if (!parse_bool(&config->name, table, STR(name), STR(table) "." STR(name), true)) {        \
            goto fail_read;                                                                        \
        }                                                                                          \
    } while (0)

#define PARSE_COLOR(table, name)                                                                   \
    do {                                                                                           \
        if (!parse_color(config->name, table, STR(name), STR(table) "." STR(name), true)) {        \
            goto fail_read;                                                                        \
        }                                                                                          \
    } while (0)

#define PARSE_DOUBLE(table, name)                                                                  \
    do {                                                                                           \
        if (!parse_double(&config->name, table, STR(name), STR(table) "." STR(name), true)) {      \
            goto fail_read;                                                                        \
        }                                                                                          \
    } while (0)

#define PARSE_DOUBLE_OR(table, name)                                                               \
    if (!parse_double(&config->name, table, STR(name), STR(table) "." STR(name), false))

#define PARSE_ENUM(table, name, ...)                                                               \
    do {                                                                                           \
        bool ok;                                                                                   \
        const struct mapping mappings[] = __VA_ARGS__;                                             \
        config->name = parse_enum(&ok, table, STR(name), STR(table) "." STR(name), mappings,       \
                                  ARRAY_LEN(mappings), true);                                      \
        if (!ok) {                                                                                 \
            goto fail_read;                                                                        \
        }                                                                                          \
    } while (0)

#define PARSE_INT(table, name)                                                                     \
    do {                                                                                           \
        if (!parse_int(&config->name, table, STR(name), STR(table) "." STR(name), true)) {         \
            goto fail_read;                                                                        \
        }                                                                                          \
    } while (0)

#define PARSE_INT_OR(table, name)                                                                  \
    if (!parse_int(&config->name, table, STR(name), STR(table) "." STR(name), false))

#define PARSE_STRING(table, name)                                                                  \
    do {                                                                                           \
        if (!parse_str(&config->name, table, STR(name), STR(table) "." STR(name), true)) {         \
            goto fail_read;                                                                        \
        }                                                                                          \
    } while (0)

#define PARSE_STRING_OR(table, name)                                                               \
    if (!parse_str(&config->name, table, STR(name), STR(table) "." STR(name), false))

#define CHECK_MIN_MAX(table, name, min, max)                                                       \
    do {                                                                                           \
        if ((config->name) < min) {                                                                \
            wlr_log(WLR_ERROR, "config: integer value '%s' below minimum (%d < %s)",               \
                    STR(table) "." STR(name), (config->name), STR(min));                           \
            goto fail_read;                                                                        \
        } else if ((config->name) > max) {                                                         \
            wlr_log(WLR_ERROR, "config: integer value '%s' above maximum (%d > %s)",               \
                    STR(table) "." STR(name), (config->name), STR(max));                           \
            goto fail_read;                                                                        \
        }                                                                                          \
    } while (0)

#define CHECK_MIN_MAX_DOUBLE(table, name, min, max)                                                \
    do {                                                                                           \
        if ((config->name) < min) {                                                                \
            wlr_log(WLR_ERROR, "config: double value '%s' below minimum (%lf < %s)",               \
                    STR(table) "." STR(name), (config->name), STR(min));                           \
            goto fail_read;                                                                        \
        } else if ((config->name) > max) {                                                         \
            wlr_log(WLR_ERROR, "config: double value '%s' above maximum (%lf > %s)",               \
                    STR(table) "." STR(name), (config->name), STR(max));                           \
            goto fail_read;                                                                        \
        }                                                                                          \
    } while (0)

struct config *
config_read() {
    char *path = config_get_path();
    if (!path) {
        return false;
    }
    FILE *file = fopen(path, "r");
    if (!file) {
        wlr_log_errno(WLR_ERROR, "failed to open config file (%s)", path);
        free(path);
        goto fail_file;
    }
    free(path);
    char buf[4096];
    toml_table_t *conf = toml_parse_file(file, buf, sizeof(buf));
    fclose(file);
    if (!conf) {
        wlr_log(WLR_ERROR, "failed to parse config: %s", buf);
        goto fail_parse;
    }

    struct config *config = calloc(1, sizeof(struct config));
    ww_assert(config);

    // input
    toml_table_t *input = toml_table_in(conf, "input");
    if (!input) {
        wlr_log(WLR_ERROR, "config: missing section 'input'");
        goto fail_read;
    }
    PARSE_INT(input, repeat_delay);
    CHECK_MIN_MAX(input, repeat_delay, 1, 1000);
    PARSE_INT(input, repeat_rate);
    CHECK_MIN_MAX(input, repeat_rate, 1, 100);
    PARSE_BOOL(input, confine_pointer);
    PARSE_DOUBLE(input, main_sens);
    CHECK_MIN_MAX_DOUBLE(input, main_sens, 0.01, 10.0);
    PARSE_DOUBLE_OR(input, alt_sens) {
        config->alt_sens = config->main_sens;
    }

    // appearance
    toml_table_t *appearance = toml_table_in(conf, "appearance");
    if (!appearance) {
        wlr_log(WLR_ERROR, "config: missing section 'appearance'");
        goto fail_read;
    }
    PARSE_COLOR(appearance, background_color);
    PARSE_COLOR(appearance, lock_color);
    PARSE_STRING_OR(appearance, cursor_theme) {
        config->cursor_theme = NULL;
    }
    PARSE_INT_OR(appearance, cursor_size) {
        config->cursor_size = 24;
    }
    CHECK_MIN_MAX(appearance, cursor_size, 1, 64);
    PARSE_DOUBLE(appearance, ninb_opacity);
    CHECK_MIN_MAX_DOUBLE(appearance, ninb_opacity, 0.1, 1.0);
    PARSE_ENUM(appearance, ninb_location,
               {
                   {"topleft", TOP_LEFT},
                   {"top", TOP},
                   {"top_right", TOP_RIGHT},
                   {"left", LEFT},
                   {"right", RIGHT},
                   {"bottomleft", BOTTOM_LEFT},
                   {"bottomright", BOTTOM_RIGHT},
               });

    // wall
    toml_table_t *wall = toml_table_in(conf, "wall");
    if (!wall) {
        wlr_log(WLR_ERROR, "config: missing section 'wall'");
        goto fail_read;
    }
    PARSE_INT(wall, wall_width);
    CHECK_MIN_MAX(wall, wall_width, 1, 10);
    PARSE_INT(wall, wall_height);
    CHECK_MIN_MAX(wall, wall_height, 1, 10);
    PARSE_INT(wall, stretch_width);
    CHECK_MIN_MAX(wall, stretch_width, 1, 4096);
    PARSE_INT(wall, stretch_height);
    CHECK_MIN_MAX(wall, stretch_height, 1, 4096);
    PARSE_BOOL(wall, use_f1);
    PARSE_BOOL(wall, remain_in_background);
    PARSE_INT_OR(wall, alt_width) {
        config->alt_width = -1;
    }
    PARSE_INT_OR(wall, alt_height) {
        config->alt_height = -1;
    }
    if ((config->alt_width < 0) != (config->alt_height < 0)) {
        wlr_log(WLR_ERROR, "config: only one dimension present in alternate resolution");
        goto fail_read;
    }
    if ((config->has_alt_res = config->alt_width > 0)) {
        CHECK_MIN_MAX(wall, alt_width, 1, 16384);
        CHECK_MIN_MAX(wall, alt_height, 1, 16384);
    }

    // reset
    toml_table_t *reset = toml_table_in(conf, "reset");
    if (!reset) {
        wlr_log(WLR_ERROR, "config: missing section 'reset'");
        goto fail_read;
    }
    PARSE_ENUM(reset, unlock_behavior,
               {
                   {"unlock", UNLOCK_ACCEPT},
                   {"remain_locked", UNLOCK_IGNORE},
                   {"reset", UNLOCK_RESET},
               });
    PARSE_BOOL(reset, count_resets);
    if (config->count_resets) {
        PARSE_STRING(reset, resets_file);
    }
    PARSE_BOOL(reset, wall_bypass);
    PARSE_INT(reset, grace_period);
    CHECK_MIN_MAX(reset, grace_period, 0, 60000);

    // performance
    toml_table_t *performance = toml_table_in(conf, "performance");
    if (performance) {
        config->has_cpu = true;
        PARSE_INT_OR(performance, idle_cpu) {
            config->has_cpu = false;
        }
        PARSE_INT_OR(performance, low_cpu) {
            config->has_cpu = false;
        }
        PARSE_INT_OR(performance, high_cpu) {
            config->has_cpu = false;
        }
        PARSE_INT_OR(performance, active_cpu) {
            config->has_cpu = false;
        }
        if (config->has_cpu) {
            PARSE_INT(performance, preview_threshold);
            CHECK_MIN_MAX(performance, idle_cpu, 1, 10000);
            CHECK_MIN_MAX(performance, low_cpu, 1, 10000);
            CHECK_MIN_MAX(performance, high_cpu, 1, 10000);
            CHECK_MIN_MAX(performance, active_cpu, 1, 10000);
            CHECK_MIN_MAX(performance, preview_threshold, 0, 100);
        }
        PARSE_STRING_OR(performance, sleepbg_lock) {
            config->sleepbg_lock = NULL;
        }
    }

    // keybinds
    toml_table_t *keybinds = toml_table_in(conf, "keybinds");
    if (!keybinds) {
        wlr_log(WLR_ERROR, "config: missing section 'keybinds'");
        goto fail_read;
    }
    if (toml_table_nkval(keybinds) > MAX_BINDS) {
        wlr_log(WLR_ERROR, "config: too many keybinds");
        goto fail_read;
    }
    for (int i = 0;; i++) {
        const char *key = toml_key_in(keybinds, i);
        if (!key) {
            break;
        }
        struct keybind *keybind = &config->binds[config->bind_count];

        char *inputs[8] = {0};
        char *keyname = strdup(key);
        for (char **input = inputs, *ptr = keyname;; ptr++) {
            if (input - inputs == 8) {
                wlr_log(WLR_ERROR, "config: too many inputs in keybind '%s'", key);
                free(keyname);
                goto fail_read;
            }

            bool exit_after = false;
            char *start = ptr;
            while (*ptr && *ptr != '-') {
                ptr++;
            }
            if (*ptr == '\0') {
                exit_after = true;
            }

            *(ptr) = '\0';
            *(input++) = start;
            if (exit_after) {
                break;
            }
        }
        bool found_button = false, found_key = false;
        for (int j = 0; j < 8; j++) {
            char *input = inputs[j];
            if (!input) {
                break;
            }
            for (unsigned long k = 0; k < ARRAY_LEN(modifiers); k++) {
                if (strcasecmp(input, modifiers[k].name) == 0) {
                    if (keybind->modifiers & modifiers[k].val) {
                        wlr_log(WLR_ERROR, "config: duplicate modifier '%s' in keybind '%s'", input,
                                key);
                        goto fail_input;
                    }
                    keybind->modifiers |= modifiers[k].val;
                    goto next_input;
                }
            }
            for (unsigned long k = 0; k < ARRAY_LEN(buttons); k++) {
                if (strcasecmp(input, buttons[k].name) == 0) {
                    if (found_button) {
                        wlr_log(WLR_ERROR, "config: more than one button in keybind '%s'", key);
                        goto fail_input;
                    } else if (found_key) {
                        wlr_log(WLR_ERROR, "config: both button and key in keybind '%s'", key);
                        goto fail_input;
                    }
                    found_button = true;
                    keybind->input.button = buttons[k].val;
                    keybind->type = BIND_MOUSE;
                    goto next_input;
                }
            }
            xkb_keysym_t sym = xkb_keysym_from_name(input, XKB_KEYSYM_CASE_INSENSITIVE);
            if (sym != XKB_KEY_NoSymbol) {
                if (found_key) {
                    wlr_log(WLR_ERROR, "config: more than one key in keybind '%s'", key);
                    goto fail_input;
                } else if (found_button) {
                    wlr_log(WLR_ERROR, "config: both button and key in keybind '%s'", key);
                    goto fail_input;
                }
                found_key = true;
                keybind->input.sym = sym;
                keybind->type = BIND_KEY;
                goto next_input;
            }

            wlr_log(WLR_ERROR, "config: unknown input '%s' in keybind '%s'", input, key);

        fail_input:
            free(keyname);
            goto fail_read;
        next_input:;
        }
        free(keyname);

        toml_array_t *array = toml_array_in(keybinds, key);
        if (array) {
            if (!parse_bind_array(array, keybind, key)) {
                goto fail_read;
            }
            keybind->allow_in_menu = true;
        } else {
            toml_table_t *table = toml_table_in(keybinds, key);
            if (!table) {
                wlr_log(WLR_ERROR, "config: invalid type for keybind '%s'", key);
                goto fail_read;
            }
            if (!parse_bind_table(table, keybind, key)) {
                goto fail_read;
            }
        }
        config->bind_count++;
    }

    toml_free(conf);
    return config;

fail_read:
    toml_free(conf);
    config_destroy(config);

fail_parse:
fail_file:
    return false;
}

void
config_destroy(struct config *config) {
    if (config->cursor_theme) {
        free(config->cursor_theme);
    }
    if (config->resets_file) {
        free(config->resets_file);
    }
    if (config->sleepbg_lock) {
        free(config->sleepbg_lock);
    }
    free(config);
}