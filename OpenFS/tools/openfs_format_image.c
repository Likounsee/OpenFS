#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "openfs/format.h"
#include "openfs/fsck.h"

#if defined(_WIN32)
#include <windows.h>
#endif

typedef struct image_context {
    FILE *file;
    uint64_t size;
    uint32_t block_size;
} image_context_t;

static int seek64(FILE *file, uint64_t offset)
{
#if defined(_WIN32)
    return _fseeki64(file, (__int64)offset, SEEK_SET) == 0;
#else
    if(offset > (uint64_t)INT64_MAX)return 0;
    return fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
}

static openfs_io_result_t image_read(void *context, uint64_t block, uint32_t count, void *buffer)
{
    image_context_t *ctx=(image_context_t *)context;
    if(ctx==NULL||ctx->file==NULL||buffer==NULL)return OPENFS_IO_INVALID_ARGUMENT;
    if(!seek64(ctx->file,block*(uint64_t)ctx->block_size))return OPENFS_IO_IO_ERROR;
    if(fread(buffer,ctx->block_size,count,ctx->file)!=count)return OPENFS_IO_IO_ERROR;
    return OPENFS_IO_OK;
}

static openfs_io_result_t image_write(void *context, uint64_t block, uint32_t count, const void *buffer)
{
    image_context_t *ctx=(image_context_t *)context;
    if(ctx==NULL||ctx->file==NULL||buffer==NULL)return OPENFS_IO_INVALID_ARGUMENT;
    if(!seek64(ctx->file,block*(uint64_t)ctx->block_size))return OPENFS_IO_IO_ERROR;
    if(fwrite(buffer,ctx->block_size,count,ctx->file)!=count)return OPENFS_IO_IO_ERROR;
    return OPENFS_IO_OK;
}

static openfs_io_result_t image_flush(void *context)
{
    image_context_t *ctx=(image_context_t *)context;
    if(ctx==NULL||ctx->file==NULL)return OPENFS_IO_INVALID_ARGUMENT;
    return fflush(ctx->file)==0?OPENFS_IO_OK:OPENFS_IO_IO_ERROR;
}

static int parse_u64(const char *s,uint64_t *out)
{
    char *end=NULL;
    unsigned long long v;
    if(s==NULL||out==NULL||*s=='\0')return 0;
    v=strtoull(s,&end,10);
    if(end==s||*end!='\0'||v==0U)return 0;
    *out=(uint64_t)v;
    return 1;
}

static int parse_u32(const char *s,uint32_t *out)
{
    uint64_t v=0U;
    if(!parse_u64(s,&v)||v>UINT32_MAX)return 0;
    *out=(uint32_t)v;
    return 1;
}

static void make_uuid(uint8_t uuid[16])
{
    uint64_t a=(uint64_t)rand()^((uint64_t)rand()<<32U);
    uint64_t b=(uint64_t)rand()^((uint64_t)rand()<<32U);
    memcpy(uuid,&a,8U);
    memcpy(uuid+8U,&b,8U);
    uuid[6]=(uint8_t)((uuid[6]&0x0fU)|0x40U);
    uuid[8]=(uint8_t)((uuid[8]&0x3fU)|0x80U);
}

int main(int argc,char **argv)
{
    const char *path;
    uint64_t blocks=0U;
    uint32_t block_size=65536U;
    uint64_t bytes=0U;
    FILE *file;
    image_context_t ctx;
    openfs_block_device_t device;
    uint8_t uuid[16];
    openfs_format_result_t result;

    if(argc<3||argc>4){
        fprintf(stderr,"Usage: %s <image> <size_bytes> [block_size]\n",argv[0]);
        return 2;
    }
    path=argv[1];
    if(!parse_u64(argv[2],&bytes)||bytes==0U){
        fprintf(stderr,"Invalid image size.\n");
        return 2;
    }
    if(argc==4&&!parse_u32(argv[3],&block_size)){
        fprintf(stderr,"Invalid block size.\n");
        return 2;
    }
    if(block_size<OPENFS_MIN_BLOCK_SIZE||block_size>OPENFS_MAX_BLOCK_SIZE||(block_size&(block_size-1U))!=0U){
        fprintf(stderr,"Block size must be a power of two from %u to %u.\n",OPENFS_MIN_BLOCK_SIZE,OPENFS_MAX_BLOCK_SIZE);
        return 2;
    }
    if(bytes%block_size!=0U){
        fprintf(stderr,"Image size must be a multiple of the block size.\n");
        return 2;
    }
    blocks=bytes/block_size;
    if(blocks<64U){
        fprintf(stderr,"Invalid image size; minimum is 64 blocks at the selected block size.\n");
        return 2;
    }
    file=fopen(path,"w+b");
    if(file==NULL){
        perror("fopen");
        return 1;
    }
    if(!seek64(file,bytes-1U)||fputc(0,file)==EOF||fflush(file)!=0){
        fprintf(stderr,"Could not allocate image file.\n");
        fclose(file);
        return 1;
    }

    memset(&ctx,0,sizeof(ctx));
    ctx.file=file;
    ctx.size=bytes;
    ctx.block_size=block_size;

    memset(&device,0,sizeof(device));
    device.context=&ctx;
    device.block_size=block_size;
    device.block_count=blocks;
    device.read=image_read;
    device.write=image_write;
    device.flush=image_flush;

    make_uuid(uuid);
    result=openfs_format(&device,uuid);
    if(result!=OPENFS_FORMAT_OK){
        fprintf(stderr,"OpenFS format failed: %d\n",(int)result);
        fclose(file);
        return 1;
    }

    {
        openfs_superblock_t superblock;
        uint64_t checked=0U;
        result=openfs_read_superblock(&device,&superblock);
        if(result!=OPENFS_FORMAT_OK||superblock.total_blocks!=blocks||superblock.block_size!=block_size||
           memcmp(superblock.uuid,uuid,sizeof(uuid))!=0){
            fprintf(stderr,"OpenFS format verification failed: %d\n",(int)result);
            fclose(file);
            return 1;
        }
        if(openfs_fsck(&device,&superblock,&checked)!=OPENFS_FSCK_OK){
            fprintf(stderr,"OpenFS format fsck verification failed.\n");
            fclose(file);
            return 1;
        }
    }
    if(fclose(file)!=0){
        fprintf(stderr,"OpenFS image close failed.\n");
        return 1;
    }

    printf("OpenFS image formatted successfully.\n");
    printf("Image: %s\n",path);
    printf("Size: %" PRIu64 " bytes\n",bytes);
    printf("Block size: %" PRIu32 " bytes\n",block_size);
    printf("Blocks: %" PRIu64 "\n",blocks);
    return 0;
}
