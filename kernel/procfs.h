// procfs: /proc pseudo filesystem, per-process info generated on read
#pragma once
#include "vfs.h"

// build the (single) procfs root vnode; mp_parent is the directory the
// root will be mounted over, used to resolve ".." from /proc
struct vnode *procfs_mount(struct vnode *mp_parent);
