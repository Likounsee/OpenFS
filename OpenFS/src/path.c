    openfs_dir_entry_t existing;openfs_dir_result_t dr=openfs_dir_lookup(d,s,&parent_inode,name,&existing);
    if(dr==OPENFS_DIR_OK)return OPENFS_PATH_EXISTS;if(dr!=OPENFS_DIR_NOT_FOUND)return map_dir_result(dr);
    uint64_t source_ino=0U;r=openfs_path_lookup_follow(d,s,source_path,&source_ino);if(r!=OPENFS_PATH_OK)return r;
    openfs_inode_t source;r=read_inode(d,s,source_ino,&source);if(r!=OPENFS_PATH_OK)return r;
    if((source.mode&OPENFS_INODE_TYPE_MASK)!=OPENFS_INODE_MODE_REGULAR)return OPENFS_PATH_INVALID_ARGUMENT;
    uint64_t clone_ino=0U;
    openfs_cow_result_t cr=openfs_cow_clone_inode(d,s,&source,parent,&clone_ino);
    if(cr!=OPENFS_COW_OK)return map_cow_result(cr);
    openfs_inode_t clone;
    r=read_inode(d,s,clone_ino,&clone);
    if(r!=OPENFS_PATH_OK){
        /*
         * The clone inode is not safely discardable without its persisted
         * generation and extents. In particular, passing an uninitialized
         * inode here would turn an I/O error into undefined behavior.
         * Transaction callers can roll the complete operation back; for a
         * direct caller, preserve the original read error instead.
         */
        return r;
    }
    openfs_dir_entry_t entry={clone_ino,clone.generation,1U};
    dr=openfs_dir_add(d,s,&parent_inode,name,&entry);
    if(dr!=OPENFS_DIR_OK){
        if(openfs_cow_discard_inode(d,s,&clone)!=OPENFS_COW_OK)return OPENFS_PATH_CORRUPT;
        return map_dir_result(dr);
    }
    *out=clone_ino;return OPENFS_PATH_OK;
}