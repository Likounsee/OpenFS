    uint8_t marker=0xE7U;
    uint64_t write_offset=(count-1U)*(uint64_t)d.block_size+123U;
    assert(openfs_file_write(&v,&fallback.superblock,&recovered,write_offset,&marker,1U)==OPENFS_FILE_OK);
    data[write_offset]=marker;
    assert(openfs_unmount(&fallback)==OPENFS_MOUNT_OK);

    openfs_mount_t remounted;
    assert(openfs_mount(&remounted,&v)==OPENFS_MOUNT_OK);
    assert(openfs_inode_read(&v,remounted.superblock.inode_table_start,ino_no,inode_count,&recovered)==OPENFS_INODE_OK);
    assert(recovered.size==bytes&&recovered.blocks==count);
    got=0U;
    assert(openfs_file_read(&v,&remounted.superblock,&recovered,write_offset,readback+write_offset,1U,&got)==OPENFS_FILE_OK);
    assert(got==1U&&readback[write_offset]==marker);
    uint64_t fsck_errors=0U;assert(openfs_fsck(&v,&remounted.superblock,&fsck_errors)==OPENFS_FSCK_OK&&fsck_errors==0U);
    assert(openfs_unmount(&remounted)==OPENFS_MOUNT_OK);
    free(readback);free(data);free(d.bytes);
}

static void cow_clone_extent_tree_regression(void)
{
    disk_t d={.block_size=4096U,.block_count=512U};
    d.bytes=calloc((size_t)d.block_count,d.block_size);assert(d.bytes);
    openfs_block_device_t v={&d,d.block_size,d.block_count,rd,wr,fl};
    uint8_t uuid[16]={0x49U};assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
    openfs_mount_t m;assert(openfs_mount(&m,&v)==OPENFS_MOUNT_OK);
    uint64_t source_ino=0U;assert(openfs_path_create(&v,&m.superblock,"/tree-source",OPENFS_INODE_MODE_REGULAR|0644U,&source_ino)==OPENFS_PATH_OK);
    uint64_t inode_count=(m.superblock.inode_table_blocks*(uint64_t)m.superblock.block_size)/OPENFS_INODE_SIZE;
    openfs_inode_t source;assert(openfs_inode_read(&v,m.superblock.inode_table_start,source_ino,inode_count,&source)==OPENFS_INODE_OK);
    uint8_t payload[4096U];uint64_t spacers[OPENFS_INODE_TREE_INLINE_EXTENT_MAX]={0U};
    for(uint64_t n=0U;n<OPENFS_INODE_TREE_INLINE_EXTENT_MAX+2U;n++){
        memset(payload,(int)(0x30U+n),sizeof(payload));
        assert(openfs_file_write(&v,&m.superblock,&source,n*(uint64_t)d.block_size,payload,sizeof(payload))==OPENFS_FILE_OK);
        assert(openfs_inode_read(&v,m.superblock.inode_table_start,source_ino,inode_count,&source)==OPENFS_INODE_OK);
        if(n<OPENFS_INODE_TREE_INLINE_EXTENT_MAX)assert(openfs_alloc_block(&v,&m.superblock,&spacers[n])==OPENFS_ALLOC_OK);
    }
    assert(source.extent_count==OPENFS_INODE_TREE_INLINE_EXTENT_MAX+2U);
    for(unsigned i=0U;i<OPENFS_INODE_TREE_INLINE_EXTENT_MAX;i++)assert(openfs_free_block(&v,&m.superblock,spacers[i])==OPENFS_ALLOC_OK);
    uint64_t source_root=openfs_inode_get_extent_tree_root(&source);assert(source_root!=0U);
    uint64_t clone_ino=0U;assert(openfs_path_clone(&v,&m.superblock,"/tree-source","/tree-clone",&clone_ino)==OPENFS_PATH_OK);
    openfs_inode_t clone;assert(openfs_inode_read(&v,m.superblock.inode_table_start,clone_ino,inode_count,&clone)==OPENFS_INODE_OK);
    uint64_t clone_root=openfs_inode_get_extent_tree_root(&clone);assert(clone_root!=0U&&clone_root!=source_root);
    assert(clone.extent_count==source.extent_count&&clone.blocks==source.blocks&&clone.size==source.size);
    for(uint64_t logical=0U;logical<source.blocks;logical++){
        uint64_t a=0U,b=0U;assert(openfs_file_map_block_device(&v,&m.superblock,&source,logical,&a)==OPENFS_FILE_OK);assert(openfs_file_map_block_device(&v,&m.superblock,&clone,logical,&b)==OPENFS_FILE_OK);assert(a==b);
        uint16_t refs=0U;assert(openfs_cow_refcount_get(&v,&m.superblock,a,&refs)==OPENFS_COW_OK&&refs==2U);
    }
    uint64_t errors=0U;assert(openfs_fsck(&v,&m.superblock,&errors)==OPENFS_FSCK_OK&&errors==0U);
    assert(openfs_path_unlink(&v,&m.superblock,"/tree-clone")==OPENFS_PATH_OK);
    for(uint64_t logical=0U;logical<source.blocks;logical++){
        uint64_t a=0U;assert(openfs_file_map_block_device(&v,&m.superblock,&source,logical,&a)==OPENFS_FILE_OK);uint16_t refs=0U;assert(openfs_cow_refcount_get(&v,&m.superblock,a,&refs)==OPENFS_COW_OK&&refs==1U);
    }
    assert(openfs_fsck(&v,&m.superblock,&errors)==OPENFS_FSCK_OK&&errors==0U);
    assert(openfs_path_unlink(&v,&m.superblock,"/tree-source")==OPENFS_PATH_OK);
    assert(openfs_unmount(&m)==OPENFS_MOUNT_OK);free(d.bytes);
}

static void cow_clone_reference_integrity_regression(void)
{
    disk_t d={.block_size=4096U,.block_count=256U};
    d.bytes=calloc((size_t)d.block_count,d.block_size);assert(d.bytes);
    openfs_block_device_t v={&d,d.block_size,d.block_count,rd,wr,fl};
    uint8_t uuid[16]={0x48U};assert(openfs_format(&v,uuid)==OPENFS_FORMAT_OK);
    openfs_mount_t m;assert(openfs_mount(&m,&v)==OPENFS_MOUNT_OK);
    uint64_t source_ino=0U;assert(openfs_path_create(&v,&m.superblock,"/source",OPENFS_INODE_MODE_REGULAR|0644U,&source_ino)==OPENFS_PATH_OK);
    openfs_inode_t source;uint64_t inode_count=(m.superblock.inode_table_blocks*(uint64_t)m.superblock.block_size)/OPENFS_INODE_SIZE;
    assert(openfs_inode_read(&v,m.superblock.inode_table_start,source_ino,inode_count,&source)==OPENFS_INODE_OK);
    uint8_t payload[4096U];memset(payload,0xA7U,sizeof(payload));
    assert(openfs_file_write(&v,&m.superblock,&source,0U,payload,sizeof(payload))==OPENFS_FILE_OK);
    assert(openfs_inode_read(&v,m.superblock.inode_table_start,source_ino,inode_count,&source)==OPENFS_INODE_OK);
    openfs_extent_t source_extent;assert(openfs_inode_get_extent(&source,0U,&source_extent)==OPENFS_EXTENT_OK);
    openfs_journal_t clone_journal;assert(openfs_journal_open(&clone_journal,&v,&m.superblock)==OPENFS_JOURNAL_OK);
    openfs_transaction_t clone_tx;uint64_t tx_clone_ino=0U;
    assert(openfs_transaction_begin(&clone_tx,&v,&clone_journal)==OPENFS_TRANSACTION_OK);
    assert(openfs_path_clone_tx(&clone_tx,&m.superblock,"/source","/tx-abort",&tx_clone_ino)==OPENFS_PATH_OK);
    assert(openfs_transaction_abort(&clone_tx)==OPENFS_TRANSACTION_OK);
    assert(openfs_path_lookup(&v,&m.superblock,"/tx-abort",&tx_clone_ino)==OPENFS_PATH_NOT_FOUND);
    uint16_t refs_after_abort=0U;assert(openfs_cow_refcount_get(&v,&m.superblock,source_extent.physical_start,&refs_after_abort)==OPENFS_COW_OK&&refs_after_abort==1U);
    assert(openfs_transaction_begin(&clone_tx,&v,&clone_journal)==OPENFS_TRANSACTION_OK);
    assert(openfs_path_clone_tx(&clone_tx,&m.superblock,"/source","/tx-commit",&tx_clone_ino)==OPENFS_PATH_OK);
    assert(openfs_transaction_commit(&clone_tx)==OPENFS_TRANSACTION_OK);
    assert(openfs_cow_refcount_get(&v,&m.superblock,source_extent.physical_start,&refs_after_abort)==OPENFS_COW_OK&&refs_after_abort==2U);