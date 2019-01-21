#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <ctype.h>
#include "kernel/errno.h"
#include "fs/tty.h"

extern struct tty_driver pty_slave;

// the master holds a reference to the slave, so the slave will always be cleaned up second
// when the master cleans up it hangs up the slave, making any operation that references the master unreachable

static int pty_master_init(struct tty *tty) {
    struct tty *slave = tty_alloc(&pty_slave, tty->num);
    slave->refcount = 1;
    pty_slave.ttys[tty->num] = slave;
    tty->pty.other = slave;
    slave->pty.other = tty;
    slave->pty.locked = true;
    return 0;
}

static void pty_master_cleanup(struct tty *tty) {
    struct tty *slave = tty->pty.other;
    slave->pty.other = NULL;
    lock(&slave->lock);
    tty_hangup(slave);
    unlock(&slave->lock);
    tty_release(slave);
}

static int pty_slave_open(struct tty *tty) {
    if (tty->pty.other == NULL)
        return _EIO;
    if (tty->pty.locked)
        return _EIO;
    return 0;
}

static int pty_ioctl(struct tty *tty, int cmd, void *arg) {
    struct tty *slave = tty->pty.other;
    switch (cmd) {
        case TIOCSPTLCK_:
            slave->pty.locked = !!*(dword_t *) arg;
            break;
        case TIOCGPTN_:
            *(dword_t *) arg = slave->num;
            break;
        default:
            return _EINVAL;
    }
    return 0;
}

static int pty_write(struct tty *tty, const void *buf, size_t len, bool blocking) {
    return tty_input(tty->pty.other, buf, len, blocking);
}

static int pty_return_eio(struct tty *UNUSED(tty)) {
    return _EIO;
}

#define MAX_PTYS (1 << 12)

const struct tty_driver_ops pty_master_ops = {
    .init = pty_master_init,
    .open = pty_return_eio,
    .write = pty_write,
    .ioctl = pty_ioctl,
    .cleanup = pty_master_cleanup,
};
DEFINE_TTY_DRIVER(pty_master, &pty_master_ops, MAX_PTYS);

const struct tty_driver_ops pty_slave_ops = {
    .init = pty_return_eio,
    .open = pty_slave_open,
    .write = pty_write,
};
DEFINE_TTY_DRIVER(pty_slave, &pty_slave_ops, MAX_PTYS);

int ptmx_open(struct fd *fd) {
    int pty_num;
    lock(&ttys_lock);
    for (pty_num = 0; pty_num < MAX_PTYS; pty_num++) {
        if (pty_slave.ttys[pty_num] == NULL)
            break;
    }
    unlock(&ttys_lock);
    if (pty_num == MAX_PTYS)
        return _ENOSPC;

    struct tty *tty = tty_get(&pty_master, pty_num);
    if (IS_ERR(tty))
        return PTR_ERR(tty);

    fd->tty = tty;
    lock(&tty->fds_lock);
    list_add(&tty->fds, &fd->other_fds);
    unlock(&tty->fds_lock);
    return 0;
}

static bool isdigits(const char *str) {
    for (int i = 0; str[i] != '\0'; i++)
        if (!isdigit(str[i]))
            return false;
    return true;
}

static const struct fd_ops devpts_fdops;

static bool devpts_pty_exists(int pty_num) {
    if (pty_num < 0 || pty_num > MAX_PTYS)
        return false;
    lock(&ttys_lock);
    bool exists = pty_slave.ttys[pty_num] != NULL;
    unlock(&ttys_lock);
    return exists;
}

// this has a slightly weird error returning convention
// I'm lucky that ENOENT is -2 and not -1
static int devpts_get_pty_num(const char *path) {
    if (strcmp(path, "") == 0)
        return -1; // root
    if (path[0] != '/' || path[1] == '\0' || strchr(path + 1, '/') != NULL)
        return _ENOENT;

    // there's one path component here, which had better be a pty number
    const char *name = path + 1; // skip the initial /
    if (!isdigits(name))
        return _ENOENT;
    // it's not possible to correctly use atoi
    long pty_long = atol(name);
    if (pty_long > INT_MAX)
        return _ENOENT;
    int pty_num = (int) pty_long;
    if (!devpts_pty_exists(pty_num))
        return _ENOENT;
    return pty_num;
}

static struct fd *devpts_open(struct mount *UNUSED(mount), const char *path, int UNUSED(flags), int UNUSED(mode)) {
    int pty_num = devpts_get_pty_num(path);
    if (pty_num == _ENOENT)
        return ERR_PTR(_ENOENT);
    struct fd *fd = fd_create(&devpts_fdops);
    fd->pty_num = pty_num;
    return fd;
}

static int devpts_getpath(struct fd *fd, char *buf) {
    if (fd->pty_num == -1)
        strcpy(buf, "");
    else
        sprintf(buf, "/%d", fd->pty_num);
    return 0;
}

static void devpts_stat_num(int pty_num, struct statbuf *stat) {
    if (pty_num == -1) {
        // root
        stat->mode = S_IFDIR | 0755;
        stat->inode = 1;
    } else {
        lock(&ttys_lock);
        struct tty *tty = pty_slave.ttys[pty_num];
        assert(tty != NULL);
        lock(&tty->lock);

        stat->mode = S_IFCHR | tty->pty.perms;
        stat->uid = tty->pty.uid;
        stat->gid = tty->pty.gid;
        stat->inode = pty_num + 3;
        stat->rdev = dev_make(TTY_PSEUDO_SLAVE_MAJOR, pty_num);

        unlock(&tty->lock);
        unlock(&ttys_lock);
    }
}

static int devpts_fstat(struct fd *fd, struct statbuf *stat) {
    devpts_stat_num(fd->pty_num, stat);
    return 0;
}

static int devpts_stat(struct mount *UNUSED(mount), const char *path, struct statbuf *stat, bool UNUSED(follow_links)) {
    int pty_num = devpts_get_pty_num(path);
    if (pty_num == _ENOENT)
        return _ENOENT;
    devpts_stat_num(pty_num, stat);
    return 0;
}

static int devpts_readdir(struct fd *fd, struct dir_entry *entry) {
    assert(fd->pty_num == -1); // there shouldn't be anything to list but the root

    int pty_num = fd->offset - 1;
    do {
        pty_num++;
    } while (pty_num < MAX_PTYS && !devpts_pty_exists(pty_num));
    if (pty_num >= MAX_PTYS)
        return 0;
    sprintf(entry->name, "%d", pty_num);
    entry->inode = pty_num + 3;
    return 1;
}

const struct fs_ops devptsfs = {
    .name = "devpts", .magic = 0x1cd1,
    .open = devpts_open,
    .getpath = devpts_getpath,
    .stat = devpts_stat,
    .fstat = devpts_fstat,
};

static const struct fd_ops devpts_fdops = {
    .readdir = devpts_readdir,
};