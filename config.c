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

static const char config_name[] = "/waywall.toml";

static char *
get_config_path() {
    char *path = NULL;
    const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    if (xdg_config_home) {
        size_t len = strlen(xdg_config_home);
        path = malloc(len + strlen(config_name) + 1);
        ww_assert(path);
        strcpy(path, xdg_config_home);
        strcat(path, config_name);
        return path;
    }

    const char *home = getenv("HOME");
    if (home) {
        size_t len = strlen(home);
        path = malloc(len + strlen(config_name) + 1);
        ww_assert(path);
        strcpy(path, home);
        strcat(path, config_name);
        return path;
    }

    wlr_log(WLR_ERROR, "no suitable directory found for config file");
    return NULL;
}

static bool
parse_bool(bool *value, toml_table_t *table, const char *value_name, const char *full_name) {
    toml_datum_t datum = toml_bool_in(table, value_name);
    if (!datum.ok) {
        wlr_log(WLR_ERROR, "config: missing boolean value '%s'", full_name);
        return false;
    }
    *value = datum.u.b;
    return true;
}

static bool
parse_int(int *value, toml_table_t *table, const char *value_name, const char *full_name) {
    toml_datum_t datum = toml_int_in(table, value_name);
    if (!datum.ok) {
        wlr_log(WLR_ERROR, "config: missing integer value '%s'", full_name);
        return false;
    }
    *value = datum.u.i;
    return true;
}

static bool
parse_str(char **value, toml_table_t *table, const char *value_name, const char *full_name) {
    toml_datum_t datum = toml_string_in(table, value_name);
    if (!datum.ok) {
        wlr_log(WLR_ERROR, "config: missing string value '%s'", full_name);
        return false;
    }
    *value = datum.u.s;
    return true;
}

/*
 * Sorry for the preprocessor crimes.
 * These should only be called from `config_read`, where `config` is in scope.
 * The names of config values in the file and in the C struct must be identical.
 * The names of the tables in the file and the associated `toml_table_t *` values must be identical.
 */
#define PARSE_VALUE(table, name)                                                                   \
    do {                                                                                           \
        bool ret = _Generic((config->name), bool: parse_bool, int: parse_int, char *: parse_str)(  \
            &(config->name), table, WW_STRINGIFY(name),                                            \
            WW_STRINGIFY(table) "." WW_STRINGIFY(name));                                           \
        if (!ret) {                                                                                \
            goto fail_read;                                                                        \
        }                                                                                          \
    } while (0)

#define CHECK_MIN_MAX(table, name, min, max)                                                       \
    do {                                                                                           \
        if ((config->name) < min) {                                                                \
            wlr_log(WLR_ERROR, "config: integer value '%s' below minimum (%d < %s)",               \
                    WW_STRINGIFY(table) "." WW_STRINGIFY(name), (config->name),                    \
                    WW_STRINGIFY(min));                                                            \
            goto fail_read;                                                                        \
        } else if ((config->name) > max) {                                                         \
            wlr_log(WLR_ERROR, "config: integer value '%s' above maximum (%d > %s)",               \
                    WW_STRINGIFY(table) "." WW_STRINGIFY(name), (config->name),                    \
                    WW_STRINGIFY(max));                                                            \
            goto fail_read;                                                                        \
        }                                                                                          \
    } while (0)

struct config *
config_read() {
    char *path = get_config_path();
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

    // input
    toml_table_t *input = toml_table_in(conf, "input");
    if (!input) {
        wlr_log(WLR_ERROR, "config: missing section 'input'");
        goto fail_read;
    }
    PARSE_VALUE(input, repeat_delay);
    CHECK_MIN_MAX(input, repeat_delay, 1, 1000);
    PARSE_VALUE(input, repeat_rate);
    CHECK_MIN_MAX(input, repeat_rate, 1, 100);

    // appearance
    toml_table_t *appearance = toml_table_in(conf, "appearance");
    if (!appearance) {
        wlr_log(WLR_ERROR, "config: missing section 'appearance'");
        goto fail_read;
    }
    { // background color
        toml_datum_t datum = toml_string_in(appearance, "background_color");
        if (!datum.ok) {
            wlr_log(WLR_ERROR, "config: missing string value 'background_color'");
            goto fail_read;
        }
        char *background_color = datum.u.s;
        size_t len = strlen(background_color);
        bool maybe_valid = (len == 6) || (len == 7 && background_color[0] == '#');
        if (!maybe_valid) {
            wlr_log(WLR_ERROR, "config: invalid background color '%s'", background_color);
            free(background_color);
            goto fail_read;
        }
        int r, g, b;
        int n = sscanf(background_color[0] == '#' ? background_color + 1 : background_color,
                       "%02x%02x%02x", &r, &g, &b);
        if (n != 3) {
            wlr_log(WLR_ERROR, "config: invalid background color '%s'", background_color);
            free(background_color);
            goto fail_read;
        }
        free(background_color);
        config->background_color[0] = r / 255.0;
        config->background_color[1] = g / 255.0;
        config->background_color[2] = b / 255.0;
        config->background_color[3] = 1.0;
    }

    // wall
    toml_table_t *wall = toml_table_in(conf, "wall");
    if (!wall) {
        wlr_log(WLR_ERROR, "config: missing section 'wall'");
        goto fail_read;
    }
    PARSE_VALUE(wall, wall_width);
    CHECK_MIN_MAX(wall, wall_width, 1, 10);
    PARSE_VALUE(wall, wall_height);
    CHECK_MIN_MAX(wall, wall_height, 1, 10);
    PARSE_VALUE(wall, stretch_width);
    CHECK_MIN_MAX(wall, stretch_width, 1, 4096);
    PARSE_VALUE(wall, stretch_height);
    CHECK_MIN_MAX(wall, stretch_height, 1, 4096);
    PARSE_VALUE(wall, use_f1);

    // reset
    toml_table_t *reset = toml_table_in(conf, "reset");
    if (!reset) {
        wlr_log(WLR_ERROR, "config: missing section 'reset'");
        goto fail_read;
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
        if (!array) {
            wlr_log(WLR_ERROR, "config: found non-array value at keybind '%s'", key);
            goto fail_read;
        }
        if (toml_array_nelem(array) > MAX_ACTIONS) {
            wlr_log(WLR_ERROR, "config: too many actions assigned to keybind '%s'", key);
            goto fail_read;
        } else if (toml_array_nelem(array) == 0) {
            wlr_log(WLR_ERROR, "config: no actions assigned to keybind '%s'", key);
            goto fail_read;
        }
        for (int j = 0; j < toml_array_nelem(array); j++) {
            toml_datum_t action = toml_string_at(array, j);
            if (!action.ok) {
                wlr_log(WLR_ERROR, "config: found non-string value at index %d of keybind '%s'", j,
                        key);
                free(action.u.s);
                goto fail_read;
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
                wlr_log(WLR_ERROR, "config: unknown action '%s' assigned to keybind '%s'",
                        action.u.s, key);
                free(action.u.s);
                goto fail_read;
            }
            free(action.u.s);
        }
        config->bind_count++;
    }

    toml_free(conf);
    return config;

fail_read:
    toml_free(conf);
    free(config);

fail_parse:
fail_file:
    return false;
}

void
config_destroy(struct config *config) {}
