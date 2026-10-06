#ifndef OPENFS_FD_H
#define OPENFS_FD_H
#include <stdint.h>
#include <stddef.h>
#include "openfs/file.h"
#include "openfs/lock.h"
#ifdef __cplusplus
extern "C" {
#endif

#define OPENFS_FD_RDONLY 0x00000000U
#define OPENFS_FD_WRONLY 0x00000001U
#define OPENFS_FD_RDWR   0x00000002U
#define OPENFS_FD_ACCESS_MASK 0x00000003U
#define OPENFS_FD_CREAT  0x00000040U
#define OPENFS_FD_EXCL   0x00000080U
#define OPENFS_FD_TRUNC  0x00000200U
#define OPENFS_FD_APPEND 0x00000400U

typedef enum {
    OPENFS_FD_OK=0, OPENFS_FD_INVALID_ARGUMENT=1, OPENFS_FD_NOT_FOUND=2,
    OPENFS_FD_EXISTS=3, OPENFS_FD_ACCESS_DENIED=4, OPENFS_FD_CLOSED=5,
    OPENFS_FD_BAD_SEEK=6, OPENFS_FD_IO_ERROR=7, OPENFS_FD_CORRUPT=8
} openfs_fd_result_t;

typedef struct openfs_file_handle {
    openfs_block_device_t *device;
    openfs_superblock_t superblock;
    openfs_inode_t inode;
    uint64_t offset;
    uint32_t flags;
    uint32_t references;
    int closed;
    openfs_mutex_t lock;
    int lock_initialized;
} openfs_file_handle_t;

openfs_fd_result_t openfs_fd_open(openfs_block_device_t *,const openfs_superblock_t *,const char *,uint32_t,uint32_t,openfs_file_handle_t **);
openfs_fd_result_t openfs_fd_open_as(openfs_block_device_t *,const openfs_superblock_t *,const char *,uint32_t,uint32_t,uint32_t,uint32_t,openfs_file_handle_t **);
openfs_fd_result_t openfs_fd_retain(openfs_file_handle_t *);
openfs_fd_result_t openfs_fd_close(openfs_file_handle_t *);
openfs_fd_result_t openfs_fd_dup(openfs_file_handle_t *,openfs_file_handle_t **);
openfs_fd_result_t openfs_fd_read(openfs_file_handle_t *,void *,size_t,size_t *);
openfs_fd_result_t openfs_fd_write(openfs_file_handle_t *,const void *,size_t);
openfs_fd_result_t openfs_fd_seek(openfs_file_handle_t *,int64_t,int,int64_t *);
openfs_fd_result_t openfs_fd_truncate(openfs_file_handle_t *,uint64_t);
openfs_fd_result_t openfs_fd_stat(openfs_file_handle_t *,openfs_inode_t *);
uint64_t openfs_fd_inode_number(const openfs_file_handle_t *);

#ifdef __cplusplus
}
#endif
#endif