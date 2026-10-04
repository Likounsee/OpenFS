#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "openfs/crc32c.h"
#include "openfs/format.h"
#include "openfs/fsck.h"
#include "openfs/journal.h"
#include "openfs/mount.h"
#include "openfs/path.h"
#include "openfs/file.h"
#include "openfs/inode.h"
#include "openfs/transaction.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define BS 4096U
#define BC 256U
#define CRASH_EXIT 137

typedef enum {
    J1_BEGIN_PARTIAL = 0,
    J2_DATA_PARTIAL,
    J3_BETWEEN_DATA,
    J4_DATA_PARTIAL_OFFSETS,
    J5_COMMIT_PARTIAL,
    C1_BEFORE_CLEAR,
    C2_CLEAR_MID,
    C3_AFTER_CLEAR_BEFORE_FLUSH,
    C4_CHECKPOINT_FLUSH,
    C6_MULTI_CHECKPOINT
} cut_t;

typedef struct {
    FILE *f;
    uint64_t blocks;
    uint64_t js;
    uint64_t jb;
    cut_t cut;
    unsigned journal_data_writes;
    unsigned zero_writes;
    unsigned flushes;
    int armed;
} disk_t;

static void crash_now(void) {
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), CRASH_EXIT);
#else
    _exit(CRASH_EXIT);
#endif
}

static int seek_block(disk_t *d, uint64_t b) {
    uint64_t off = b * (uint64_t)BS;
    if (b >= d->blocks || off > (uint64_t)LONG_MAX) return 0;
    return fseek(d->f, (long)off, SEEK_SET) == 0;
}

static int flush_file(disk_t *d) { return fflush(d->f) == 0; }

static int journal_block(const disk_t *d, uint64_t b) {
    return b >= d->js && b < d->js + d->jb;
}

static openfs_io_result_t rd(void *ctx, uint64_t first, uint32_t count, void *out) {
    disk_t *d = ctx;
    if (!d || !out || !count || first >= d->blocks ||
        (uint64_t)count > d->blocks - first || !seek_block(d, first))
        return OPENFS_IO_OUT_OF_RANGE;
    size_t n = (size_t)((uint64_t)count * BS);
    return fread(out, 1U, n, d->f) == n ? OPENFS_IO_OK : OPENFS_IO_IO_ERROR;
}

static openfs_io_result_t wr(void *ctx, uint64_t first, uint32_t count, const void *in) {
    disk_t *d = ctx;
    if (!d || !in || !count || first >= d->blocks ||
        (uint64_t)count > d->blocks - first || !seek_block(d, first))
        return OPENFS_IO_OUT_OF_RANGE;

    size_t n = (size_t)((uint64_t)count * BS);
    const uint8_t *raw = (const uint8_t *)in;
    int isj = count == 1U && journal_block(d, first);
    int iszero = isj;
    if (iszero) {
        for (size_t i = 0; i < n; ++i) {
            if (raw[i] != 0U) { iszero = 0; break; }
        }
    }

    if (d->armed && count == 1U && isj && memcmp(raw, OPENFS_JOURNAL_MAGIC, 5U) == 0) {
        uint8_t type = raw[5U];
        if (d->cut == J1_BEGIN_PARTIAL && type == OPENFS_JOURNAL_BEGIN) {
            size_t prefix = 24U;
            if (fwrite(raw, 1U, prefix, d->f) != prefix || !flush_file(d)) return OPENFS_IO_IO_ERROR;
            crash_now();
        }
        if ((d->cut == J2_DATA_PARTIAL || d->cut == J4_DATA_PARTIAL_OFFSETS) &&
            type == OPENFS_JOURNAL_DATA && d->journal_data_writes == 0U) {
            size_t prefix = d->cut == J2_DATA_PARTIAL ? 32U : 2048U;
            if (fwrite(raw, 1U, prefix, d->f) != prefix || !flush_file(d)) return OPENFS_IO_IO_ERROR;
            crash_now();
        }
        if (d->cut == J3_BETWEEN_DATA && type == OPENFS_JOURNAL_DATA) {
            d->journal_data_writes++;
            if (d->journal_data_writes == 2U) crash_now();
        }
        if (d->cut == J5_COMMIT_PARTIAL && type == OPENFS_JOURNAL_COMMIT) {
            size_t prefix = 16U;
            if (fwrite(raw, 1U, prefix, d->f) != prefix || !flush_file(d)) return OPENFS_IO_IO_ERROR;
            crash_now();
        }
    }

    if (d->armed && count == 1U && isj && iszero &&
        d->cut == C1_BEFORE_CLEAR && d->zero_writes == 0U) {
        crash_now();
    }

    if (fwrite(in, 1U, n, d->f) != n) return OPENFS_IO_IO_ERROR;
    if (!flush_file(d)) return OPENFS_IO_IO_ERROR;

    if (d->armed && count == 1U && isj && iszero) {
        d->zero_writes++;
        if (d->cut == C1_BEFORE_CLEAR && d->zero_writes == 1U) crash_now();
        if (d->cut == C2_CLEAR_MID && d->zero_writes == 3U) crash_now();
        if (d->cut == C3_AFTER_CLEAR_BEFORE_FLUSH && d->zero_writes == d->jb) crash_now();
    }
    return OPENFS_IO_OK;
}

static openfs_io_result_t fl(void *ctx) {
    disk_t *d = ctx;
    if (!d) return OPENFS_IO_INVALID_ARGUMENT;
    if (!flush_file(d)) return OPENFS_IO_IO_ERROR;
    d->flushes++;
    if (d->armed && ((d->cut == C4_CHECKPOINT_FLUSH && d->flushes == 3U) ||
                     (d->cut == C6_MULTI_CHECKPOINT && d->flushes == 6U))) crash_now();
    return OPENFS_IO_OK;
}

static int create_disk(const char *path, disk_t *d) {
    memset(d, 0, sizeof(*d));
    d->blocks = BC;
    d->f = fopen(path, "w+b");
    if (!d->f) return 0;
    if (fseek(d->f, (long)(BS * (uint64_t)BC - 1U), SEEK_SET) != 0 ||
        fputc(0, d->f) == EOF || !flush_file(d)) return 0;
    return 1;
}

static int open_disk(const char *path, disk_t *d) {
    memset(d, 0, sizeof(*d));
    d->blocks = BC;
    d->f = fopen(path, "r+b");
    return d->f != NULL;
}

static void close_disk(disk_t *d) {
    if (d->f) { (void)fflush(d->f); (void)fclose(d->f); d->f = NULL; }
}

static openfs_block_device_t dev(disk_t *d) {
    openfs_block_device_t v = { d, BS, BC, rd, wr, fl };
    return v;
}

static int prepare(const char *path, uint64_t *target) {
    disk_t d;
    if (!create_disk(path, &d)) return 0;
    openfs_block_device_t v = dev(&d);
    uint8_t uuid[16] = {0x50U,0x32U};
    if (openfs_format(&v, uuid) != OPENFS_FORMAT_OK) { close_disk(&d); return 0; }
    openfs_superblock_t s;
    if (openfs_read_superblock(&v, &s) != OPENFS_FORMAT_OK) { close_disk(&d); return 0; }
    if (target) *target = s.data_start + 8U;
    close_disk(&d);
    return 1;
}

static int tx_worker(const char *path, cut_t cut, int multi) {
    disk_t d;
    if (!open_disk(path, &d)) return 2;
    openfs_block_device_t v = dev(&d);
    openfs_superblock_t s;
    if (openfs_read_superblock(&v, &s) != OPENFS_FORMAT_OK) return 3;
    d.js = s.journal_start; d.jb = s.journal_blocks;
    d.cut = cut; d.armed = cut == C6_MULTI_CHECKPOINT ? 0 : 1;

    openfs_journal_t j;
    if (openfs_journal_open(&j, &v, &s) != OPENFS_JOURNAL_OK) return 4;
    for (int pass = 0; pass < (multi ? 2 : 1); ++pass) {
        if (multi && pass == 1) d.armed = 1;
        openfs_transaction_t t;
        if (openfs_transaction_begin(&t, &v, &j) != OPENFS_TRANSACTION_OK) return 5;
        openfs_block_device_t *td = openfs_transaction_device(&t);
        uint8_t a[BS], b[BS];
        memset(a, (uint8_t)(0xA0U + (unsigned)pass), sizeof(a));
        memset(b, (uint8_t)(0xB0U + (unsigned)pass), sizeof(b));
        if (!td || td->write(td->context, s.data_start + 8U + (uint64_t)pass, 1U, a) != OPENFS_IO_OK)
            return 6;
        if (td->write(td->context, s.data_start + 10U + (uint64_t)pass, 1U, b) != OPENFS_IO_OK)
            return 7;
        openfs_transaction_result_t r = openfs_transaction_commit(&t);
        if (r != OPENFS_TRANSACTION_OK) return 8;
        d.cut = cut;
        if (multi && pass == 0) d.armed = 0;
    }
    close_disk(&d);
    return 0;
}

static int run_child(const char *self, const char *path, cut_t cut, int multi) {
#if defined(_WIN32)
    char cmd[1024]; STARTUPINFOA si; PROCESS_INFORMATION pi; DWORD code = 0U;
    if (snprintf(cmd, sizeof(cmd), "\"%s\" worker %d %d \"%s\"", self, (int)cut, multi, path) < 0) return 0;
    memset(&si,0,sizeof(si)); memset(&pi,0,sizeof(pi)); si.cb=sizeof(si);
    if (!CreateProcessA(NULL,cmd,NULL,NULL,FALSE,0,NULL,NULL,&si,&pi)) return 0;
    (void)WaitForSingleObject(pi.hProcess,INFINITE);
    (void)GetExitCodeProcess(pi.hProcess,&code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code == CRASH_EXIT;
#else
    pid_t pid=fork(); int st=0;
    if(pid<0)return 0;
    if(pid==0)_exit(tx_worker(path,cut,multi));
    if(waitpid(pid,&st,0)!=pid)return 0;
    return WIFEXITED(st)&&WEXITSTATUS(st)==CRASH_EXIT;
#endif
}

static int verify_corrupt(const char *path) {
    disk_t d; if (!open_disk(path, &d)) return 0;
    openfs_block_device_t v = dev(&d);
    openfs_mount_t m;
    openfs_mount_result_t r = openfs_mount(&m, &v);
    close_disk(&d);
    return r == OPENFS_MOUNT_CORRUPT;
}

static int verify(const char *path, int expect0, int expect1, int expect2, int expect3) {
    disk_t d; if(!open_disk(path,&d))return 0;
    d.armed = 0;
    openfs_block_device_t v=dev(&d); openfs_mount_t m;
    openfs_mount_result_t mr = openfs_mount(&m,&v);
    if(mr!=OPENFS_MOUNT_OK){ fprintf(stderr,"verify mount=%d\\n",(int)mr); close_disk(&d); return 0; }
    uint8_t b[BS]; int ok=1;
    uint64_t targets[4]={m.superblock.data_start+8U,m.superblock.data_start+9U,
                         m.superblock.data_start+10U,m.superblock.data_start+11U};
    int ex[4]={expect0,expect1,expect2,expect3};
    for(int i=0;i<4;i++){
        if(rd(&d,targets[i],1U,b)!=OPENFS_IO_OK){ok=0;break;}
        uint8_t want=0U;
        if (ex[i]) {
            static const uint8_t wants[4] = {0xA0U,0xA1U,0xB0U,0xB1U};
            want=wants[i];
        }
        uint8_t expected[BS]; memset(expected,want,sizeof(expected));
        if(memcmp(b,expected,sizeof(b))!=0){ fprintf(stderr,"block mismatch i=%d want=%02x got=%02x\\n",i,want,b[0]); ok=0;break;}
    }
    uint64_t errors=0U;
    if(openfs_fsck(&v,&m.superblock,&errors)!=OPENFS_FSCK_OK||errors!=0U){ fprintf(stderr,"fsck failed errors=%llu\\n",(unsigned long long)errors); ok=0;}
    openfs_journal_t j;
    if(openfs_journal_open(&j,&v,&m.superblock)!=OPENFS_JOURNAL_OK ||
       j.next_record!=0U || j.active_transaction_id!=0U || j.commit_record_written!=0U){ fprintf(stderr,"journal not clean next=%llu active=%llu commit=%u\\n",(unsigned long long)j.next_record,(unsigned long long)j.active_transaction_id,(unsigned)j.commit_record_written); ok=0;}
    if(openfs_unmount(&m)!=OPENFS_MOUNT_OK){ fprintf(stderr,"unmount failed\\n"); ok=0; }
    openfs_mount_t m2;
    if(openfs_mount(&m2,&v)!=OPENFS_MOUNT_OK){ fprintf(stderr,"second mount failed\\n"); ok=0; }
    if(ok){
        errors=0U;
        if(openfs_fsck(&v,&m2.superblock,&errors)!=OPENFS_FSCK_OK||errors!=0U)ok=0;
        if(openfs_unmount(&m2)!=OPENFS_MOUNT_OK)ok=0;
    }
    close_disk(&d); return ok;
}

static int journal_crash_case(const char *self, cut_t cut, int multi, unsigned id) {
    char path[256];
    if(snprintf(path,sizeof(path),"openfs-journal-fine-%u.img",id)<0)return 0;
    uint64_t target=0U;
    if(!prepare(path,&target))return 0;
    int crashed=run_child(self,path,cut,multi);
    int ok=crashed;
    if(!crashed) fprintf(stderr,"fine cut did not crash: %d\\n",(int)cut);
    if(ok){
        if(cut==J1_BEGIN_PARTIAL||cut==J2_DATA_PARTIAL||
           cut==J4_DATA_PARTIAL_OFFSETS||cut==J5_COMMIT_PARTIAL)
            ok=verify_corrupt(path);
        else if(cut==J3_BETWEEN_DATA)
            ok=verify(path,0,0,0,0);
        else if(multi) ok=verify(path,1,1,1,1);
        else ok=verify(path,1,0,1,0);
    }
    if(!ok) fprintf(stderr,"fine cut verification failed: %d\\n",(int)cut);
    (void)remove(path);
    return ok;
}

static void p64(uint8_t *p,uint64_t v){for(unsigned i=0;i<8U;i++)p[i]=(uint8_t)(v>>(8U*i));}
static void p32(uint8_t *p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8U);p[2]=(uint8_t)(v>>16U);p[3]=(uint8_t)(v>>24U);}

static void raw_record(uint8_t *b,uint8_t type,uint64_t tx,uint64_t seq,const uint8_t *payload,uint32_t len){
    memset(b,0,BS);memcpy(b,OPENFS_JOURNAL_MAGIC,5U);b[5]=type;
    p64(b+8U,tx);p64(b+16U,seq);p32(b+24U,len);
    if(len)memcpy(b+32U,payload,len);
    p32(b+28U,0U);p32(b+28U,openfs_crc32c(b,BS-4U));
}

static int write_raw(openfs_block_device_t *v,const openfs_superblock_t *s,uint64_t slot,
                     uint8_t type,uint64_t tx,uint64_t seq,const uint8_t *payload,uint32_t len){
    uint8_t b[BS]; raw_record(b,type,tx,seq,payload,len);
    return v->write(v->context,s->journal_start+slot,1U,b)==OPENFS_IO_OK;
}

static int replay_state_case(int kind, unsigned id) {
    char path[256]; if(snprintf(path,sizeof(path),"openfs-replay-%u.img",id)<0)return 0;
    disk_t d; if(!create_disk(path,&d))return 0;
    openfs_block_device_t v=dev(&d); uint8_t uuid[16]={0x52U};
    if(openfs_format(&v,uuid)!=OPENFS_FORMAT_OK){close_disk(&d);return 0;}
    openfs_superblock_t s; if(openfs_read_superblock(&v,&s)!=OPENFS_FORMAT_OK){close_disk(&d);return 0;}
    uint64_t ino=0U;
    uint64_t inode_count=(s.inode_table_blocks*(uint64_t)s.block_size)/OPENFS_INODE_SIZE;
    openfs_inode_t inode;
    if(openfs_path_create(&v,&s,"/replay-target",OPENFS_INODE_MODE_REGULAR,&ino)!=OPENFS_PATH_OK ||
       openfs_inode_read(&v,s.inode_table_start,ino,inode_count,&inode)!=OPENFS_INODE_OK)
    { close_disk(&d); remove(path); return 0; }
    uint64_t target=0U;
    uint8_t initial[BS]; memset(initial,0x11U,sizeof(initial));
    if(openfs_file_write(&v,&s,&inode,0U,initial,sizeof(initial))!=OPENFS_FILE_OK ||
       openfs_file_map_block_device(&v,&s,&inode,0U,&target)!=OPENFS_FILE_OK)
    { close_disk(&d); remove(path); return 0; }
    uint8_t payload[BS-32U]; memset(payload,0,sizeof(payload));
    memcpy(payload,"OJBD1",5U); p64(payload+8U,target); p32(payload+16U,0U); p32(payload+20U,BS-56U);
    memset(payload+24U,(uint8_t)(0x60U+kind),BS-32U-24U);

    int ok=1;
    if(kind==0){ /* incomplete only */ 
        ok&=write_raw(&v,&s,0,OPENFS_JOURNAL_BEGIN,1U,1U,NULL,0U);
        ok&=write_raw(&v,&s,1,OPENFS_JOURNAL_DATA,1U,2U,payload,BS-32U);
    } else if(kind==1){ /* committed only */
        ok&=write_raw(&v,&s,0,OPENFS_JOURNAL_BEGIN,1U,1U,NULL,0U);
        ok&=write_raw(&v,&s,1,OPENFS_JOURNAL_DATA,1U,2U,payload,BS-32U);
        ok&=write_raw(&v,&s,2,OPENFS_JOURNAL_COMMIT,1U,3U,NULL,0U);
    } else if(kind==2){ /* two committed transactions */
        ok&=write_raw(&v,&s,0,OPENFS_JOURNAL_BEGIN,1U,1U,NULL,0U);
        ok&=write_raw(&v,&s,1,OPENFS_JOURNAL_DATA,1U,2U,payload,BS-32U);
        ok&=write_raw(&v,&s,2,OPENFS_JOURNAL_COMMIT,1U,3U,NULL,0U);
        memset(payload+24U,0x72U,BS-32U-24U);
        ok&=write_raw(&v,&s,3,OPENFS_JOURNAL_BEGIN,2U,4U,NULL,0U);
        ok&=write_raw(&v,&s,4,OPENFS_JOURNAL_DATA,2U,5U,payload,BS-32U);
        ok&=write_raw(&v,&s,5,OPENFS_JOURNAL_COMMIT,2U,6U,NULL,0U);
    } else if(kind==3){ /* committed then incomplete */
        ok&=write_raw(&v,&s,0,OPENFS_JOURNAL_BEGIN,1U,1U,NULL,0U);
        ok&=write_raw(&v,&s,1,OPENFS_JOURNAL_DATA,1U,2U,payload,BS-32U);
        ok&=write_raw(&v,&s,2,OPENFS_JOURNAL_COMMIT,1U,3U,NULL,0U);
        ok&=write_raw(&v,&s,3,OPENFS_JOURNAL_BEGIN,2U,4U,NULL,0U);
        ok&=write_raw(&v,&s,4,OPENFS_JOURNAL_DATA,2U,5U,payload,BS-32U);
    }
    if(!ok){close_disk(&d);remove(path);return 0;}
    close_disk(&d);

    if(!open_disk(path,&d))return 0; v=dev(&d); openfs_mount_t m;
    openfs_mount_result_t mr=openfs_mount(&m,&v);
    int expected_mount = kind==0 ? OPENFS_MOUNT_OK : OPENFS_MOUNT_OK;
    if(mr!=expected_mount) { fprintf(stderr,"replay kind=%d mount=%d\\n",kind,(int)mr); ok=0; }
    if(ok){
        uint8_t b[BS]; if(rd(&d,target,1U,b)!=OPENFS_IO_OK)ok=0;
        if(kind==0) { uint8_t z[BS]; memset(z,0x11U,BS); if(memcmp(b,z,BS)!=0)ok=0; }
        else if(kind==1) { uint8_t z[BS]={0}; memset(z,0x61U,BS-56U); if(memcmp(b,z,BS)!=0)ok=0; }
        else if(kind==2) { uint8_t z[BS]={0}; memset(z,0x72U,BS-56U); if(memcmp(b,z,BS)!=0)ok=0; }
        else { uint8_t z[BS]={0}; memset(z,0x63U,BS-56U); if(memcmp(b,z,BS)!=0)ok=0; }
        if (!ok) fprintf(stderr,"replay kind=%d block=%02x expected=%02x\\n",kind,b[0],kind==0?0x11:kind==1?0x61:kind==2?0x72:0x63);
        uint64_t e=0U; if(openfs_fsck(&v,&m.superblock,&e)!=OPENFS_FSCK_OK||e!=0U){ fprintf(stderr,"replay kind=%d fsck=%llu\\n",kind,(unsigned long long)e); ok=0; }
        if(openfs_unmount(&m)!=OPENFS_MOUNT_OK){ fprintf(stderr,"replay kind=%d unmount failed\\n",kind); ok=0; }
        openfs_mount_t m2; if(openfs_mount(&m2,&v)!=OPENFS_MOUNT_OK){ fprintf(stderr,"replay kind=%d second mount failed\\n",kind); ok=0; }
        if(ok){
            e=0U;
            if(openfs_fsck(&v,&m2.superblock,&e)!=OPENFS_FSCK_OK||e!=0U){ fprintf(stderr,"replay kind=%d second fsck=%llu\\n",kind,(unsigned long long)e); ok=0; }
            if(openfs_unmount(&m2)!=OPENFS_MOUNT_OK){ fprintf(stderr,"replay kind=%d second unmount failed\\n",kind); ok=0; }
        }
    }
    close_disk(&d); remove(path); return ok;
}

int main(int argc,char **argv){
    if(argc==5 && strcmp(argv[1],"worker")==0){
        long c=strtol(argv[2],NULL,10),m=strtol(argv[3],NULL,10);
        return tx_worker(argv[4],(cut_t)c,(int)m);
    }
    if(argc!=1)return 1;
    unsigned id=0U;
    for(int c=J1_BEGIN_PARTIAL;c<=J5_COMMIT_PARTIAL;c++) { int ok=journal_crash_case(argv[0],(cut_t)c,0,id++); if(!ok) fprintf(stderr,"journal cut failed: %d\\n",c); assert(ok); }
    for(int c=C1_BEFORE_CLEAR;c<=C4_CHECKPOINT_FLUSH;c++) {
        int ok = journal_crash_case(argv[0],(cut_t)c,0,id++);
        if(!ok) fprintf(stderr,"checkpoint cut failed: %d\\n",c);
        assert(ok);
    }
    { int ok = journal_crash_case(argv[0],C6_MULTI_CHECKPOINT,1,id++); if(!ok) fprintf(stderr,"checkpoint cut failed: C6\\n"); assert(ok); }
    for(int k=0;k<4;k++) { int ok=replay_state_case(k,id++); if(!ok) fprintf(stderr,"replay case failed: %d\\n",k); assert(ok); }
    return 0;
}
