#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>
#include "openfs_windows_adapter.h"
#include "openfs/format.h"
#include "openfs/fsck.h"
#include "openfs/file.h"
#include "openfs/inode.h"
#include "openfs/mount.h"
#include "openfs/path.h"

static int parse_bs(const wchar_t *s,uint32_t *out){wchar_t *e=NULL;unsigned long long v;if(!s||!out)return 0;v=wcstoull(s,&e,10);if(e==s||*e||v>UINT32_MAX)return 0;*out=(uint32_t)v;return 1;}
static int lookup(openfs_block_device_t *d,const openfs_superblock_t *s,const char *p,uint64_t *i){return openfs_path_lookup(d,s,p,i)==OPENFS_PATH_OK;}
static int verify(openfs_block_device_t *d,const openfs_superblock_t *s,uint64_t ino,const uint8_t *want,size_t len){
 openfs_inode_t in;uint8_t *gotbuf=(uint8_t*)malloc(len);size_t got=0;
 if(!gotbuf)return 0;
 if(openfs_inode_read(d,s->inode_table_start,(s->inode_table_blocks*(uint64_t)s->block_size)/OPENFS_INODE_SIZE,ino,&in)!=OPENFS_INODE_OK){free(gotbuf);return 0;}
 if(openfs_file_read(d,s,&in,0,gotbuf,len,&got)!=OPENFS_FILE_OK||got!=len||memcmp(gotbuf,want,len)!=0){free(gotbuf);return 0;}
 free(gotbuf);return 1;
}
int wmain(int argc,wchar_t **argv){
 openfs_windows_adapter_t a;openfs_block_device_t *d;openfs_mount_t m;openfs_inode_t in;uint32_t bs=65536;uint64_t ino=0,dir=0,checked=0;size_t len=131072;uint8_t *data=NULL;openfs_path_result_t pr;openfs_fsck_result_t fr;int ok=0;
 memset(&m,0,sizeof(m));if(argc<2||argc>3){wprintf(L"Usage: openfs-usb-test.exe <device> [block_size]\n");return 2;}if(argc==3&&!parse_bs(argv[2],&bs))return 2;
 memset(&a,0,sizeof(a));a.handle=INVALID_HANDLE_VALUE;if(openfs_windows_adapter_open(&a,argv[1],bs,1)!=OPENFS_WINDOWS_ADAPTER_OK){fwprintf(stderr,L"Cannot open device; run as Administrator.\n");return 3;}d=openfs_windows_adapter_device(&a);
 if(openfs_mount(&m,d)!=OPENFS_MOUNT_OK){fwprintf(stderr,L"Mount failed.\n");goto done;}wprintf(L"Mounted: %llu blocks, %u-byte blocks.\n",(unsigned long long)m.superblock.total_blocks,m.superblock.block_size);
 fr=openfs_fsck(d,&m.superblock,&checked);if(fr!=OPENFS_FSCK_OK){fwprintf(stderr,L"Initial fsck failed: %d\n",fr);goto unmount;}
 pr=openfs_path_mkdir(d,&m.superblock,"/openfs-usb-test",&dir);if(pr!=OPENFS_PATH_OK&&pr!=OPENFS_PATH_EXISTS)goto unmount;if(!lookup(d,&m.superblock,"/openfs-usb-test",&dir))goto unmount;
 pr=openfs_path_create(d,&m.superblock,"/openfs-usb-test/data.bin",OPENFS_INODE_MODE_REGULAR|0644U,&ino);if(pr!=OPENFS_PATH_OK){fwprintf(stderr,L"create failed: %d\n",pr);goto unmount;}
 data=(uint8_t*)malloc(len);if(!data)goto unmount;for(size_t i=0;i<len;i++)data[i]=(uint8_t)((i*37U+11U)&255U);
 if(openfs_inode_read(d,m.superblock.inode_table_start,(m.superblock.inode_table_blocks*(uint64_t)m.superblock.block_size)/OPENFS_INODE_SIZE,ino,&in)!=OPENFS_INODE_OK)goto unmount;
 if(openfs_file_write(d,&m.superblock,&in,0,data,len)!=OPENFS_FILE_OK||!verify(d,&m.superblock,ino,data,len)){fwprintf(stderr,L"write/read verification failed.\n");goto unmount;}
 if(openfs_path_rename(d,&m.superblock,"/openfs-usb-test/data.bin","/openfs-usb-test/data-renamed.bin")!=OPENFS_PATH_OK)goto unmount;
 if(!lookup(d,&m.superblock,"/openfs-usb-test/data-renamed.bin",&ino))goto unmount;
 if(openfs_path_unlink(d,&m.superblock,"/openfs-usb-test/data-renamed.bin")!=OPENFS_PATH_OK)goto unmount;
 if(openfs_path_unlink(d,&m.superblock,"/openfs-usb-test")!=OPENFS_PATH_NOT_EMPTY)goto unmount;
 if(openfs_path_unlink(d,&m.superblock,"/openfs-usb-test")!=OPENFS_PATH_OK)goto unmount;
 if(openfs_sync(&m)!=OPENFS_MOUNT_OK)goto unmount;if(openfs_unmount(&m)!=OPENFS_MOUNT_OK)goto done;
 if(openfs_mount(&m,d)!=OPENFS_MOUNT_OK){fwprintf(stderr,L"Remount failed.\n");goto done;}fr=openfs_fsck(d,&m.superblock,&checked);if(fr!=OPENFS_FSCK_OK){fwprintf(stderr,L"Post-remount fsck failed: %d\n",fr);goto unmount;}wprintf(L"OpenFS device test passed.\n");ok=1;
unmount:if(m.mounted)openfs_unmount(&m);
done:free(data);openfs_windows_adapter_close(&a);return ok?0:4;
}