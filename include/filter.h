/*  
    AVFS: A Virtual File System Library
    Copyright (C) 1998  Miklos Szeredi (mszeredi@inf.bme.hu)
    
    This file can be distributed either under the GNU LGPL, or under
    the GNU GPL. See the file COPYING.LIB and COPYING. 
*/

#include "avfs.h"

int av_init_filt(struct vmodule *module, const char *name,
                 const char *prog[], const char *revprog[],
                 struct ext_info *exts, struct avfs **resp);
