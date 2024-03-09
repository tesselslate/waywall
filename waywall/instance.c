#include "instance.h"
#include "server/ui.h"
#include "util.h"
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>
#include <zip.h>

struct str {
    char data[PATH_MAX];
    size_t len;
};

static bool
str_append(struct str *dst, const char *src) {
    size_t len = strlen(src);

    if (dst->len + len >= PATH_MAX - 1) {
        return false;
    }

    strcpy(dst->data + dst->len, src);
    dst->data[dst->len + len] = '\0';
    dst->len += len;
    return true;
}

static int
check_subdirs(const char *dirname) {
    static const char *should_exist[] = {
        "logs",
        "resourcepacks",
        "saves",
        "screenshots",
    };

    DIR *dir = opendir(dirname);
    if (!dir) {
        ww_log_errno(LOG_ERROR, "failed to open instance dir '%s'", dirname);
        return 1;
    }

    size_t found = 0;
    struct dirent *dirent;
    while ((dirent = readdir(dir))) {
        for (size_t i = 0; i < STATIC_ARRLEN(should_exist); i++) {
            if (strcmp(dirent->d_name, should_exist[i]) == 0) {
                found += 1;
                break;
            }
        }
    }

    closedir(dir);
    if (found != STATIC_ARRLEN(should_exist)) {
        ww_log(LOG_ERROR, "potential instance dir at '%s' is missing directories", dirname);
        return 1;
    }
    return 0;
}

static int
process_mod_zip(const char *path, struct instance_mods *mods) {
    int err;
    zip_t *zip = zip_open(path, ZIP_RDONLY, &err);
    if (!zip) {
        zip_error_t error;
        zip_error_init_with_code(&error, err);
        ww_log(LOG_ERROR, "failed to read mod zip '%s': %s", path, zip_error_strerror(&error));
        zip_error_fini(&error);
        return 1;
    }

    for (int64_t i = 0; i < zip_get_num_entries(zip, 0); i++) {
        zip_stat_t stat;
        if (zip_stat_index(zip, i, 0, &stat) != 0) {
            zip_error_t *error = zip_get_error(zip);
            ww_log(LOG_ERROR, "failed to stat entry %" PRIi64 " of '%s': %s", i, path,
                   zip_error_strerror(error));
            zip_close(zip);
            return 1;
        }

        if (strcmp(stat.name, "me/voidxwalker/autoreset/") == 0) {
            // Atum
            mods->atum = true;
            break;
        } else if (strcmp(stat.name, "com/kingcontaria/standardsettings/") == 0) {
            // StandardSettings
            mods->standard_settings = true;
            break;
        } else if (strcmp(stat.name, "me/voidxwalker/worldpreview") == 0) {
            // WorldPreview
            mods->world_preview = true;
            continue;
        } else if (strcmp(stat.name, "me/voidxwalker/worldpreview/StateOutputHelper.class") == 0) {
            // WorldPreview with state output (3.x - 4.x)
            mods->state_output = true;
            mods->world_preview = true;
            break;
        } else if (strcmp(stat.name, "xyz/tildejustin/stateoutput/") == 0) {
            // Legacy state-output
            mods->state_output = true;
            break;
        } else if (strcmp(stat.name, "dev/tildejustin/stateoutput/") == 0) {
            // state-output
            mods->state_output = true;
            break;
        }
    }

    zip_close(zip);
    return 0;
}

static int
get_mods(const char *dirname, struct instance_mods *mods) {
    struct str dirbuf = {0};
    if (!str_append(&dirbuf, dirname)) {
        ww_log(LOG_ERROR, "instance path too long");
        return 1;
    }
    if (!str_append(&dirbuf, "/mods/")) {
        ww_log(LOG_ERROR, "instance path too long");
        return 1;
    }

    DIR *dir = opendir(dirbuf.data);
    if (!dir) {
        ww_log_errno(LOG_ERROR, "failed to open instance mods dir '%s'", dirbuf.data);
        return 1;
    }

    struct dirent *dirent;
    while ((dirent = readdir(dir))) {
        if (dirent->d_name[0] == '.') {
            continue;
        }

        const char *ext = strrchr(dirent->d_name, '.');
        if (!ext || strcmp(ext, ".jar") != 0) {
            continue;
        }

        struct str modbuf = {0};
        if (!str_append(&modbuf, dirbuf.data)) {
            ww_log(LOG_ERROR, "instance path too long");
            closedir(dir);
            return 1;
        }
        if (!str_append(&modbuf, dirent->d_name)) {
            ww_log(LOG_ERROR, "instance path too long");
            closedir(dir);
            return 1;
        }

        if (process_mod_zip(modbuf.data, mods) != 0) {
            closedir(dir);
            return 1;
        }
    }

    closedir(dir);
    return 0;
}

static int
get_version(struct server_view *view) {
    char *title = server_view_get_title(view);

    // TODO: Support snapshots?

    int version[3], n;
    if (strncmp(title, "Minecraft ", 10) == 0) {
        n = sscanf(title, "Minecraft %2d.%2d.%2d", &version[0], &version[1], &version[2]);
    } else if (strncmp(title, "Minecraft* ", 11) == 0) {
        n = sscanf(title, "Minecraft* %2d.%2d.%2d", &version[0], &version[1], &version[2]);
    } else {
        ww_log(LOG_ERROR, "failed to parse window title '%s'", title);
        free(title);
        return -1;
    }

    if (n != 3) {
        ww_log(LOG_ERROR, "failed to parse window title '%s'", title);
        free(title);
        return -1;
    }

    free(title);
    return version[1];
}

struct instance *
instance_create(struct server_view *view) {
    static_assert(sizeof(pid_t) <= sizeof(int));

    pid_t pid = server_view_get_pid(view);

    char buf[PATH_MAX];
    ssize_t n = snprintf(buf, STATIC_ARRLEN(buf), "/proc/%d/cwd", (int)pid);
    ww_assert(n < (ssize_t)STATIC_ARRLEN(buf));

    char dir[PATH_MAX];
    n = readlink(buf, dir, STATIC_ARRLEN(dir));
    if (n < 0) {
        ww_log_errno(LOG_ERROR, "failed to readlink instance dir (pid=%d)", (int)pid);
        return NULL;
    } else if (n >= (ssize_t)STATIC_ARRLEN(dir) - 1) {
        ww_log(LOG_ERROR, "instance dir too long (pid=%d)", (int)pid);
        return NULL;
    }
    dir[n] = '\0';

    // If this is a real Minecraft instance, it should have some normal directories. This does not
    // guarantee that it is actually an instance, but if you're trying to fool the detection then
    // it's your fault.
    if (check_subdirs(dir) != 0) {
        return NULL;
    }

    struct instance_mods mods = {0};
    if (get_mods(dir, &mods) != 0) {
        return NULL;
    }

    int version = get_version(view);
    if (version < 0) {
        return NULL;
    }

    struct instance *instance = calloc(1, sizeof(*instance));
    if (!instance) {
        ww_log(LOG_ERROR, "failed to allocate instance");
        return NULL;
    }

    instance->dir = strdup(dir);
    instance->pid = pid;
    instance->mods = mods;
    instance->version = version;
    instance->view = view;

    return instance;
}

void
instance_destroy(struct instance *instance) {
    free(instance->dir);
    free(instance);
}
