#include "cpu/cgroup_setup.h"
#include "util.h"
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * cgroup_setup_check is designed to be run regardless of privilege level.
 * cgroup_setup_root requires privileges to write to the cgroup filesystem.
 *
 * It may be helpful to see the bash script upon which cgroup_setup_root was based:
 *
 *  CGROUP_DIR=/sys/fs/cgroup/waywall
 *  USERNAME=$(logname)
 *
 *  mkdir -p $CGROUP_DIR
 *
 *  chown "$USERNAME" $CGROUP_DIR/cgroup.procs
 *  echo "+cpu" > $CGROUP_DIR/cgroup.subtree_control
 *
 *  for subgroup in idle low high active; do
 *      mkdir $CGROUP_DIR/$subgroup
 *      chown "$USERNAME" $CGROUP_DIR/$subgroup/cgroup.procs
 *      chown "$USERNAME" $CGROUP_DIR/$subgroup/cpu.weight
 *  done
 */

#define CGROUP_BASE_DIR "/sys/fs/cgroup/waywall/"
#define PERMS_MESSAGE "elevated permissions are required to setup cgroups"

static const char *subgroups[] = {
    "idle/",
    "low/",
    "high/",
    "active/",
};

static const char *files[] = {
    "cgroup.procs",
    "cpu.weight",
};

static int
get_user(uid_t *uid, gid_t *gid) {
    ww_assert(uid);
    ww_assert(gid);

    // Traditional privilege escalation binaries will end up overwriting both the UID/GID and the
    // effective UID/GID. However, it's still possible to get the username of the user who ran
    // waywall.
    const char *username = getlogin();
    if (!username) {
        ww_log_errno(LOG_ERROR, "failed to get username");
        return 1;
    }

    errno = ENOENT;
    const struct passwd *passwd = getpwnam(username);
    if (!passwd) {
        ww_log_errno(LOG_ERROR, "failed to resolve user data");
        return 1;
    }

    *uid = passwd->pw_uid;
    *gid = passwd->pw_gid;

    return 0;
}

int
cgroup_setup_check() {
    // 1. Check that the files "cgroup.procs" and "cpu.weight" are present and writable by the
    //    current user in each subgroup.
    // 2. Check that /sys/fs/cgroup/waywall/cgroup.procs is writable by the current user.

    uid_t euid = geteuid();
    gid_t egid = getegid();

    for (size_t i = 0; i < STATIC_ARRLEN(subgroups); i++) {
        for (size_t j = 0; j < STATIC_ARRLEN(files); j++) {
            struct str buf = {0};
            ww_assert(str_append(&buf, CGROUP_BASE_DIR));
            ww_assert(str_append(&buf, subgroups[i]));
            ww_assert(str_append(&buf, files[j]));

            struct stat fstat = {0};
            if (stat(buf.data, &fstat) != 0) {
                if (errno == ENOENT) {
                    return 1;
                } else {
                    ww_log_errno(LOG_ERROR, "stat '%s'", buf.data);
                    return -1;
                }
            }

            if (fstat.st_uid != euid && fstat.st_gid != egid) {
                return 1;
            }
        }
    }

    struct stat fstat = {0};
    if (stat(CGROUP_BASE_DIR "cgroup.procs", &fstat) != 0) {
        if (errno == ENOENT) {
            return 1;
        } else {
            ww_log_errno(LOG_ERROR, "stat 'cgroup.procs'");
            return -1;
        }
    }

    return 0;
}

int
cgroup_setup_root() {
    uid_t uid;
    gid_t gid;
    if (get_user(&uid, &gid) != 0) {
        return 1;
    }

    if (mkdir(CGROUP_BASE_DIR, 0755) != 0 && errno != EEXIST) {
        if (errno == EPERM || errno == EACCES) {
            ww_log(LOG_ERROR, PERMS_MESSAGE);
            return 1;
        }
        ww_log_errno(LOG_ERROR, "failed to create base cgroup directory '%s'", CGROUP_BASE_DIR);
        return 1;
    }

    int fd = open(CGROUP_BASE_DIR "cgroup.subtree_control", O_WRONLY, 0644);
    if (fd == -1) {
        if (errno == EPERM || errno == EACCES) {
            ww_log(LOG_ERROR, PERMS_MESSAGE);
            return 1;
        }
        ww_log_errno(LOG_ERROR, "failed to open 'cgroup.subtree_control'");
        return 1;
    }

    static const char subtree[] = "+cpu";
    if (write(fd, subtree, STATIC_STRLEN(subtree)) != STATIC_STRLEN(subtree)) {
        if (errno == EPERM || errno == EACCES) {
            ww_log(LOG_ERROR, PERMS_MESSAGE);
            return 1;
        }
        ww_log_errno(LOG_ERROR, "failed to write 'cgroup.subtree_control'");
        return 1;
    }

    if (chown(CGROUP_BASE_DIR "cgroup.procs", uid, gid) != 0) {
        if (errno == EPERM || errno == EACCES) {
            ww_log(LOG_ERROR, PERMS_MESSAGE);
            return 1;
        }
        ww_log_errno(LOG_ERROR, "failed to chown 'cgroup.procs'");
        return 1;
    }

    for (size_t i = 0; i < STATIC_ARRLEN(subgroups); i++) {
        const char *subgroup = subgroups[i];
        struct str buf = {0};
        ww_assert(str_append(&buf, CGROUP_BASE_DIR));
        ww_assert(str_append(&buf, subgroup));

        if (mkdir(buf.data, 0755) != 0 && errno != EEXIST) {
            if (errno == EPERM || errno == EACCES) {
                ww_log(LOG_ERROR, PERMS_MESSAGE);
                return 1;
            }
            ww_log_errno(LOG_ERROR, "failed to create subgroup directory '%s'", buf.data);
            return 1;
        }

        for (size_t j = 0; j < STATIC_ARRLEN(files); j++) {
            struct str buf2 = buf;
            ww_assert(str_append(&buf2, files[j]));

            if (chown(buf2.data, uid, gid) != 0) {
                if (errno == EPERM || errno == EACCES) {
                    ww_log(LOG_ERROR, PERMS_MESSAGE);
                    return 1;
                }
                ww_log_errno(LOG_ERROR, "failed to chown '%s'", buf2.data);
                return 1;
            }
        }
    }

    return 0;
}
