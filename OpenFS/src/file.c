            int ok=1;
            if(old_tree_block!=NULL){
                if(device->write(device->context,old_root,1U,old_tree_block)!=OPENFS_IO_OK)ok=0;
                else if(device->flush(device->context)!=OPENFS_IO_OK)ok=0;
            }
            if(shrink_tail_saved&&device->write(device->context,shrink_tail_physical,1U,shrink_tail_backup)!=OPENFS_IO_OK)ok=0;
            free(old_tree_block);free(freed);free(shrink_tail_backup);free(tail_backup);*inode=original;
            return ok?r:OPENFS_FILE_CORRUPT;
        }

        if(device->flush(device->context)!=OPENFS_IO_OK){
            int ok=1;
            if(old_tree_block!=NULL&&device->write(device->context,old_root,1U,old_tree_block)!=OPENFS_IO_OK)ok=0;
            if(write_inode(device,sb,&original)!=OPENFS_FILE_OK)ok=0;
            if(device->flush(device->context)!=OPENFS_IO_OK)ok=0;
            free(old_tree_block);free(freed);free(tail_backup);*inode=original;
            return ok?OPENFS_FILE_IO_ERROR:OPENFS_FILE_CORRUPT;
        }

        for(uint64_t n=0U;n<removed;n++){
            if(openfs_free_block(device,sb,freed[n])!=OPENFS_ALLOC_OK){
                int ok=1;
                if(old_tree_block!=NULL&&device->write(device->context,old_root,1U,old_tree_block)!=OPENFS_IO_OK)ok=0;
                for(uint64_t k=0U;k<n;k++){
                    if(openfs_bitmap_set(device,sb->block_bitmap_start,sb->block_bitmap_blocks,freed[k],1)!=OPENFS_BITMAP_OK)ok=0;
                }
                if(write_inode(device,sb,&original)!=OPENFS_FILE_OK)ok=0;
                if(device->flush(device->context)!=OPENFS_IO_OK)ok=0;
                free(old_tree_block);free(freed);free(tail_backup);*inode=original;
                return ok?OPENFS_FILE_IO_ERROR:OPENFS_FILE_CORRUPT;
            }
        }

        uint64_t new_root=openfs_inode_get_extent_tree_root(&reduced);
        int root_freed=0;
        if(old_root!=0U&&new_root!=old_root){
            if(openfs_free_block(device,sb,old_root)!=OPENFS_ALLOC_OK){
                int ok=1;
                if(old_tree_block!=NULL&&device->write(device->context,old_root,1U,old_tree_block)!=OPENFS_IO_OK)ok=0;
                for(uint64_t k=0U;k<removed;k++){
                    if(openfs_bitmap_set(device,sb->block_bitmap_start,sb->block_bitmap_blocks,freed[k],1)!=OPENFS_BITMAP_OK)ok=0;
                }
                if(write_inode(device,sb,&original)!=OPENFS_FILE_OK)ok=0;
                if(device->flush(device->context)!=OPENFS_IO_OK)ok=0;
                free(old_tree_block);free(freed);free(tail_backup);*inode=original;
                return ok?OPENFS_FILE_IO_ERROR:OPENFS_FILE_CORRUPT;
            }
            root_freed=1;
        }

        *inode=reduced;
        if(device->flush(device->context)!=OPENFS_IO_OK){
            int ok=1;
            for(uint64_t k=0U;k<removed;k++){
                if(openfs_bitmap_set(device,sb->block_bitmap_start,sb->block_bitmap_blocks,freed[k],1)!=OPENFS_BITMAP_OK)ok=0;
            }
            if(root_freed&&openfs_bitmap_set(device,sb->block_bitmap_start,sb->block_bitmap_blocks,old_root,1)!=OPENFS_BITMAP_OK)ok=0;
            if(old_root!=0U&&new_root==old_root&&old_tree_block!=NULL&&device->write(device->context,old_root,1U,old_tree_block)!=OPENFS_IO_OK)ok=0;
            if(shrink_tail_saved&&device->write(device->context,shrink_tail_physical,1U,shrink_tail_backup)!=OPENFS_IO_OK)ok=0;
            if(write_inode(device,sb,&original)!=OPENFS_FILE_OK)ok=0;
            if(device->flush(device->context)!=OPENFS_IO_OK)ok=0;
            *inode=original;
            free(old_tree_block);free(freed);free(shrink_tail_backup);free(tail_backup);
            return ok?OPENFS_FILE_IO_ERROR:OPENFS_FILE_CORRUPT;
        }
        free(old_tree_block);free(freed);free(shrink_tail_backup);free(tail_backup);
        return OPENFS_FILE_OK;
    }

    if(tail_saved){
        uint8_t *block=malloc(device->block_size);
        if(block==NULL){free(tail_backup);return OPENFS_FILE_IO_ERROR;}
        memcpy(block,tail_backup,device->block_size);
        uint32_t from=(uint32_t)(old_size%device->block_size);
        uint32_t to=(new_blocks>old_blocks)?device->block_size:(uint32_t)(new_size%device->block_size);
        memset(block+from,0,(size_t)(to-from));
        if(device->write(device->context,tail_physical,1U,block)!=OPENFS_IO_OK){
            int ok=device->write(device->context,tail_physical,1U,tail_backup)==OPENFS_IO_OK;
            if(device->flush(device->context)!=OPENFS_IO_OK)ok=0;
            free(block);free(tail_backup);return ok?OPENFS_FILE_IO_ERROR:OPENFS_FILE_CORRUPT;
        }
        free(block);
    }

    inode->size=new_size;
    uint64_t now=openfs_time_now_ns();