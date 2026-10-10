#include "openfs/journal.h"
#include "openfs/crc32c.h"
#include "openfs/runtime.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
static void p32(uint8_t*p,uint32_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8U);p[2]=(uint8_t)(v>>16U);p[3]=(uint8_t)(v>>24U);}
static void p64(uint8_t*p,uint64_t v){for(unsigned i=0;i<8U;i++)p[i]=(uint8_t)(v>>(8U*i));}
static uint32_t g32(const uint8_t*p){return (uint32_t)p[0]|((uint32_t)p[1]<<8U)|((uint32_t)p[2]<<16U)|((uint32_t)p[3]<<24U);}
static uint64_t g64(const uint8_t*p){uint64_t v=0U;for(unsigned i=0;i<8U;i++)v|=(uint64_t)p[i]<<(8U*i);return v;}static int crc_valid(uint8_t*b,uint32_t block_size){if(b==NULL||block_size<OPENFS_JOURNAL_HEADER_SIZE)return 0;uint32_t stored=g32(b+28U);p32(b+28U,0U);uint32_t full=openfs_crc32c(b,block_size);if(stored==full){p32(b+28U,stored);return 1;}if(block_size>=4U){uint32_t legacy=openfs_crc32c(b,block_size-4U);int reserved=b[block_size-4U]==0U&&b[block_size-3U]==0U&&b[block_size-2U]==0U&&b[block_size-1U]==0U;p32(b+28U,stored);return stored==legacy&&reserved;}p32(b+28U,stored);return 0;}
static int range(const openfs_block_device_t*d,const openfs_superblock_t*s){return openfs_block_device_is_valid(d)&&s!=NULL&&d->block_size>=OPENFS_JOURNAL_HEADER_SIZE&&s->block_size==d->block_size&&s->journal_blocks>0U&&s->journal_start<d->block_count&&s->journal_blocks<=d->block_count-s->journal_start;}
static openfs_journal_result_t put(openfs_block_device_t*d,const openfs_superblock_t*s,uint64_t slot,uint64_t tx,uint64_t seq,uint32_t type,const void*data,uint32_t len){if(d->block_size<OPENFS_JOURNAL_HEADER_SIZE||len>d->block_size-OPENFS_JOURNAL_HEADER_SIZE||(len!=0U&&data==NULL))return OPENFS_JOURNAL_INVALID_ARGUMENT;if(slot>=s->journal_blocks)return OPENFS_JOURNAL_FULL;uint8_t*b=calloc(1U,d->block_size);if(!b)return OPENFS_JOURNAL_IO_ERROR;memcpy(b,OPENFS_JOURNAL_MAGIC,5U);b[5]=(uint8_t)type;p64(b+8U,tx);p64(b+16U,seq);p32(b+24U,len);if(len!=0U)memcpy(b+OPENFS_JOURNAL_HEADER_SIZE,data,len);p32(b+28U,openfs_crc32c(b,d->block_size));openfs_io_result_t io=d->write(d->context,s->journal_start+slot,1U,b);free(b);return io==OPENFS_IO_OK?OPENFS_JOURNAL_OK:OPENFS_JOURNAL_IO_ERROR;}
openfs_journal_result_t openfs_journal_open(openfs_journal_t *j,
                                               const openfs_block_device_t *d,
                                               const openfs_superblock_t *s)
{
    openfs_journal_t candidate = {0};
    if (j == NULL || !range(d, s)) return OPENFS_JOURNAL_INVALID_ARGUMENT;
    if (d->block_size <= OPENFS_JOURNAL_HEADER_SIZE + 4U)
        return OPENFS_JOURNAL_INVALID_ARGUMENT;

    candidate.journal_start = s->journal_start;
    candidate.journal_blocks = s->journal_blocks;
    candidate.block_size = s->block_size;
    if (s->journal_blocks > (uint64_t)SIZE_MAX / sizeof(uint64_t))
        return OPENFS_JOURNAL_IO_ERROR;

    uint8_t *b = malloc(d->block_size);
    if (b == NULL) return OPENFS_JOURNAL_IO_ERROR;

    int gap = 0;
    uint64_t open_tx = 0U;
    uint64_t last_started_tx = 0U;
    for (uint64_t n = 0U; n < s->journal_blocks; n++) {
        if (d->read(d->context, s->journal_start + n, 1U, b) != OPENFS_IO_OK) {
            free(b);
            return OPENFS_JOURNAL_IO_ERROR;
        }
        if (memcmp(b, OPENFS_JOURNAL_MAGIC, 5U) != 0) {
            int nonzero = 0;
            for (uint32_t z = 0U; z < d->block_size; z++) {
                if (b[z] != 0U) { nonzero = 1; break; }
            }
            if (nonzero) { free(b); return OPENFS_JOURNAL_CORRUPT; }
            gap = 1;
            continue;
        }
        if (gap || !crc_valid(b, d->block_size)) {
            free(b);
            return OPENFS_JOURNAL_CORRUPT;
        }

        uint32_t len = g32(b + 24U);
        uint64_t tx = g64(b + 8U);
        uint64_t seq = g64(b + 16U);
        uint8_t type = b[5U];
        if (b[6U] != 0U || b[7U] != 0U ||
            len > d->block_size - OPENFS_JOURNAL_HEADER_SIZE ||
            tx == 0U || seq == 0U ||
            (type != OPENFS_JOURNAL_BEGIN && type != OPENFS_JOURNAL_DATA &&
             type != OPENFS_JOURNAL_COMMIT) ||
            ((type == OPENFS_JOURNAL_BEGIN || type == OPENFS_JOURNAL_COMMIT) &&
             len != 0U)) {
            free(b);
            return OPENFS_JOURNAL_CORRUPT;
        }
        if (candidate.next_record != 0U &&
            (candidate.sequence == UINT64_MAX || seq != candidate.sequence + 1U)) {
            free(b);
            return OPENFS_JOURNAL_CORRUPT;
        }

        /*
         * Validate transaction ownership while opening, not only later during
         * replay. A newer BEGIN may supersede an uncommitted transaction after
         * a crash, but DATA/COMMIT must always belong to the currently open
         * transaction. This prevents a malformed log from being reported as
         * successfully opened and then used to seed further journal writes.
         */
        if (type == OPENFS_JOURNAL_BEGIN) {
            if (tx <= last_started_tx) {
                free(b);
                return OPENFS_JOURNAL_CORRUPT;
            }
            last_started_tx = tx;
            open_tx = tx;
        } else if (type == OPENFS_JOURNAL_DATA) {
            if (open_tx != tx) {
                free(b);
                return OPENFS_JOURNAL_CORRUPT;
            }
        } else {
            if (open_tx != tx) {
                free(b);
                return OPENFS_JOURNAL_CORRUPT;
            }
            open_tx = 0U;
            candidate.commit_record_written = 1U;
        }

        if (tx > candidate.transaction_id) candidate.transaction_id = tx;
        candidate.recovery_required = 1U;
        candidate.sequence = seq;
        candidate.next_record = n + 1U;
    }
    free(b);
    *j = candidate;
    return OPENFS_JOURNAL_OK;
}
static openfs_journal_result_t journal_begin_unlocked(openfs_journal_t*j,openfs_block_device_t*d,uint64_t*tx){if(j==NULL||tx==NULL||!openfs_block_device_is_valid(d))return OPENFS_JOURNAL_INVALID_ARGUMENT;if(j->recovery_required!=0U||j->publication_in_progress!=0U)return OPENFS_JOURNAL_IO_ERROR;if(j->active_transaction_id!=0U||j->commit_record_written!=0U)return OPENFS_JOURNAL_INVALID_ARGUMENT;if(j->next_record>=j->journal_blocks)return OPENFS_JOURNAL_FULL;if(j->transaction_id==UINT64_MAX||j->sequence==UINT64_MAX)return OPENFS_JOURNAL_FULL;uint64_t transaction_before=j->transaction_id;uint64_t sequence_before=j->sequence;uint64_t slot=j->journal_start+j->next_record;uint8_t*backup=malloc(d->block_size);if(backup==NULL)return OPENFS_JOURNAL_IO_ERROR;if(d->read(d->context,slot,1U,backup)!=OPENFS_IO_OK){free(backup);return OPENFS_JOURNAL_IO_ERROR;}j->transaction_id++;j->commit_record_written=0U;openfs_superblock_t s={0};s.block_size=j->block_size;s.journal_start=j->journal_start;s.journal_blocks=j->journal_blocks;openfs_journal_result_t r=put(d,&s,j->next_record,j->transaction_id,++j->sequence,OPENFS_JOURNAL_BEGIN,NULL,0U);if(r==OPENFS_JOURNAL_OK){j->next_record++;j->active_transaction_id=j->transaction_id;*tx=j->transaction_id;free(backup);}else if(r==OPENFS_JOURNAL_IO_ERROR){int restored=d->write(d->context,slot,1U,backup)==OPENFS_IO_OK;if(restored)restored=d->flush(d->context)==OPENFS_IO_OK;free(backup);j->transaction_id=transaction_before;j->sequence=sequence_before;if(!restored){j->recovery_required=1U;return OPENFS_JOURNAL_CORRUPT;}}else free(backup);return r;}
static openfs_journal_result_t journal_write_unlocked(openfs_journal_t*j,openfs_block_device_t*d,uint64_t tx,const void*data,uint32_t len){if(j==NULL||data==NULL||!openfs_block_device_is_valid(d))return OPENFS_JOURNAL_INVALID_ARGUMENT;if(j->recovery_required!=0U||j->publication_in_progress!=0U)return OPENFS_JOURNAL_IO_ERROR;if(j->active_transaction_id==0U||tx!=j->active_transaction_id)return OPENFS_JOURNAL_INVALID_ARGUMENT;if(len>d->block_size-OPENFS_JOURNAL_HEADER_SIZE)return OPENFS_JOURNAL_INVALID_ARGUMENT;if(j->sequence==UINT64_MAX)return OPENFS_JOURNAL_FULL;if(j->next_record>=j->journal_blocks)return OPENFS_JOURNAL_FULL;openfs_superblock_t s={0};s.block_size=j->block_size;s.journal_start=j->journal_start;s.journal_blocks=j->journal_blocks;uint64_t sequence_before=j->sequence;uint64_t slot=s.journal_start+j->next_record;uint8_t*backup=malloc(d->block_size);if(backup==NULL)return OPENFS_JOURNAL_IO_ERROR;if(d->read(d->context,slot,1U,backup)!=OPENFS_IO_OK){free(backup);return OPENFS_JOURNAL_IO_ERROR;}openfs_journal_result_t r=put(d,&s,j->next_record,tx,++j->sequence,OPENFS_JOURNAL_DATA,data,len);if(r==OPENFS_JOURNAL_OK){j->next_record++;free(backup);}else if(r==OPENFS_JOURNAL_IO_ERROR){int restored=d->write(d->context,slot,1U,backup)==OPENFS_IO_OK;if(restored)restored=d->flush(d->context)==OPENFS_IO_OK;free(backup);if(!restored){j->recovery_required=1U;return OPENFS_JOURNAL_CORRUPT;}j->sequence=sequence_before;}else free(backup);return r;}
static openfs_journal_result_t journal_commit_unlocked(openfs_journal_t*j,openfs_block_device_t*d,uint64_t tx){if(j==NULL||!openfs_block_device_is_valid(d))return OPENFS_JOURNAL_INVALID_ARGUMENT;if(j->recovery_required!=0U||j->publication_in_progress!=0U)return OPENFS_JOURNAL_IO_ERROR;if(j->active_transaction_id==0U||tx!=j->active_transaction_id)return OPENFS_JOURNAL_INVALID_ARGUMENT;if(j->sequence==UINT64_MAX)return OPENFS_JOURNAL_FULL;if(j->next_record>=j->journal_blocks)return OPENFS_JOURNAL_FULL;openfs_superblock_t s={0};s.block_size=j->block_size;s.journal_start=j->journal_start;s.journal_blocks=j->journal_blocks;uint64_t commit_block=s.journal_start+j->next_record;uint8_t*backup=malloc(d->block_size);if(backup==NULL)return OPENFS_JOURNAL_IO_ERROR;if(d->read(d->context,commit_block,1U,backup)!=OPENFS_IO_OK){free(backup);return OPENFS_JOURNAL_IO_ERROR;}uint64_t sequence_before=j->sequence;openfs_journal_result_t r=put(d,&s,j->next_record,tx,++j->sequence,OPENFS_JOURNAL_COMMIT,NULL,0U);if(r==OPENFS_JOURNAL_OK){j->next_record++;j->commit_record_written=1U;j->active_transaction_id=0U;r=d->flush(d->context)==OPENFS_IO_OK?OPENFS_JOURNAL_OK:OPENFS_JOURNAL_IO_ERROR;if(r!=OPENFS_JOURNAL_OK)j->recovery_required=1U;free(backup);return r;}if(r==OPENFS_JOURNAL_IO_ERROR){int restored=d->write(d->context,commit_block,1U,backup)==OPENFS_IO_OK;if(restored)restored=d->flush(d->context)==OPENFS_IO_OK;free(backup);if(!restored){j->recovery_required=1U;return OPENFS_JOURNAL_CORRUPT;}j->sequence=sequence_before;}else free(backup);return r;}
openfs_journal_result_t openfs_journal_replay(
    const openfs_block_device_t *d,
    const openfs_superblock_t *s,
    openfs_journal_replay_fn cb,
    void *ctx)
{
    typedef struct {
        uint64_t tx;
        uint32_t len;
        size_t offset;
    } replay_item_t;

    if (!range(d, s) || cb == NULL) return OPENFS_JOURNAL_INVALID_ARGUMENT;
    if (s->journal_blocks > SIZE_MAX / sizeof(uint64_t) ||
        s->journal_blocks > SIZE_MAX / sizeof(replay_item_t) ||
        s->journal_blocks > SIZE_MAX) {
        return OPENFS_JOURNAL_CORRUPT;
    }

    openfs_journal_result_t result = OPENFS_JOURNAL_OK;
    uint8_t *b = malloc(d->block_size);
    uint64_t *txids = NULL;
    uint8_t *states = NULL;
    uint32_t *fingerprints = NULL;
    replay_item_t *items = NULL;
    uint8_t *payloads = NULL;
    size_t item_count = 0U;
    size_t payload_size = 0U;
    size_t payload_capacity = 0U;
    uint64_t tx_count = 0U;
    uint64_t last_sequence = 0U;
    uint64_t last_tx = 0U;
    uint64_t open_tx = 0U;
    int have_sequence = 0;
    int journal_gap = 0;

    if (b == NULL) return OPENFS_JOURNAL_IO_ERROR;
    txids = calloc((size_t)s->journal_blocks, sizeof(*txids));
    states = calloc((size_t)s->journal_blocks, sizeof(*states));
    fingerprints = calloc((size_t)s->journal_blocks, sizeof(*fingerprints));
    items = calloc((size_t)s->journal_blocks, sizeof(*items));
    if (txids == NULL || states == NULL || fingerprints == NULL || items == NULL) {
        result = OPENFS_JOURNAL_IO_ERROR;
        goto cleanup;
    }

    /*
     * Pass one validates the complete journal and stores a fingerprint for
     * each slot. The second pass must return the same block content before
     * any callback can run; this prevents a CRC-valid but changed payload
     * from bypassing first-pass transaction validation.
     */
    for (uint64_t n = 0U; n < s->journal_blocks; n++) {
        if (d->read(d->context, s->journal_start + n, 1U, b) != OPENFS_IO_OK) {
            result = OPENFS_JOURNAL_IO_ERROR;
            goto cleanup;
        }
        fingerprints[n] = openfs_crc32c(b, d->block_size);
        if (memcmp(b, OPENFS_JOURNAL_MAGIC, 5U) != 0) {
            int nonzero = 0;
            for (uint32_t z = 0U; z < d->block_size; z++) {
                if (b[z] != 0U) { nonzero = 1; break; }
            }
            if (nonzero) {
                result = OPENFS_JOURNAL_CORRUPT;
                goto cleanup;
            }
            journal_gap = 1;
            continue;
        }
        if (journal_gap || !crc_valid(b, d->block_size)) {
            result = OPENFS_JOURNAL_CORRUPT;
            goto cleanup;
        }

        uint32_t len = g32(b + 24U);
        uint64_t tx = g64(b + 8U);
        uint64_t seq = g64(b + 16U);
        uint8_t type = b[5U];
        if (b[6U] != 0U || b[7U] != 0U ||
            len > d->block_size - OPENFS_JOURNAL_HEADER_SIZE ||
            tx == 0U || seq == 0U ||
            (type != OPENFS_JOURNAL_BEGIN &&
             type != OPENFS_JOURNAL_DATA &&
             type != OPENFS_JOURNAL_COMMIT) ||
            ((type == OPENFS_JOURNAL_BEGIN ||
              type == OPENFS_JOURNAL_COMMIT) && len != 0U)) {
            result = OPENFS_JOURNAL_CORRUPT;
            goto cleanup;
        }
        if (have_sequence &&
            (last_sequence == UINT64_MAX || seq != last_sequence + 1U)) {
            result = OPENFS_JOURNAL_CORRUPT;
            goto cleanup;
        }
        last_sequence = seq;
        have_sequence = 1;

        uint64_t idx = 0U;
        while (idx < tx_count && txids[idx] != tx) idx++;
        if (type == OPENFS_JOURNAL_BEGIN) {
            if (tx <= last_tx || idx != tx_count ||
                tx_count >= s->journal_blocks) {
                result = OPENFS_JOURNAL_CORRUPT;
                goto cleanup;
            }
            if (open_tx != 0U) {
                uint64_t abandoned = 0U;
                while (abandoned < tx_count && txids[abandoned] != open_tx)
                    abandoned++;
                if (abandoned >= tx_count) {
                    result = OPENFS_JOURNAL_CORRUPT;
                    goto cleanup;
                }
                states[abandoned] = 3U;
                open_tx = 0U;
            }
            txids[tx_count] = tx;
            states[tx_count] = 1U;
            tx_count++;
            last_tx = tx;
            open_tx = tx;
        } else if (type == OPENFS_JOURNAL_DATA) {
            if (idx >= tx_count || states[idx] != 1U || open_tx != tx) {
                result = OPENFS_JOURNAL_CORRUPT;
                goto cleanup;
            }
        } else {
            if (idx >= tx_count || states[idx] != 1U || open_tx != tx) {
                result = OPENFS_JOURNAL_CORRUPT;
                goto cleanup;
            }
            states[idx] = 2U;
            open_tx = 0U;
        }
    }

    /*
     * Re-read and stage all committed DATA, but compare each slot with the
     * first-pass whole-block CRC32C fingerprint. Any read error or changed
     * record fails before publication. This avoids requiring a contiguous
     * allocation as large as the entire journal during recovery.
     */
    for (uint64_t n = 0U; n < s->journal_blocks; n++) {
        if (d->read(d->context, s->journal_start + n, 1U, b) != OPENFS_IO_OK) {
            result = OPENFS_JOURNAL_IO_ERROR;
            goto cleanup;
        }
        if (fingerprints[n] != openfs_crc32c(b, d->block_size)) {
            result = OPENFS_JOURNAL_CORRUPT;
            goto cleanup;
        }
        if (memcmp(b, OPENFS_JOURNAL_MAGIC, 5U) != 0) {
            int nonzero = 0;
            for (uint32_t z = 0U; z < d->block_size; z++) {
                if (b[z] != 0U) { nonzero = 1; break; }
            }
            if (nonzero) {
                result = OPENFS_JOURNAL_CORRUPT;
                goto cleanup;
            }
            continue;
        }
        if (!crc_valid(b, d->block_size)) {
            result = OPENFS_JOURNAL_CORRUPT;
            goto cleanup;
        }

        uint32_t len = g32(b + 24U);
        uint64_t tx = g64(b + 8U);
        if (len > d->block_size - OPENFS_JOURNAL_HEADER_SIZE) {
            result = OPENFS_JOURNAL_CORRUPT;
            goto cleanup;
        }
        if (b[5U] != OPENFS_JOURNAL_DATA || len == 0U) continue;

        uint64_t idx = 0U;
        while (idx < tx_count && txids[idx] != tx) idx++;
        if (idx >= tx_count) {
            result = OPENFS_JOURNAL_CORRUPT;
            goto cleanup;
        }
        if (states[idx] != 2U) continue;

        if ((size_t)len > SIZE_MAX - payload_size) {
            result = OPENFS_JOURNAL_IO_ERROR;
            goto cleanup;
        }
        size_t needed = payload_size + (size_t)len;
        if (needed > payload_capacity) {
            size_t new_capacity = payload_capacity == 0U ? needed : payload_capacity;
            while (new_capacity < needed) {
                if (new_capacity > SIZE_MAX / 2U) {
                    new_capacity = needed;
                    break;
                }
                new_capacity *= 2U;
            }
            uint8_t *grown = realloc(payloads, new_capacity);
            if (grown == NULL) {
                result = OPENFS_JOURNAL_IO_ERROR;
                goto cleanup;
            }
            payloads = grown;
            payload_capacity = new_capacity;
        }
        memcpy(payloads + payload_size,
               b + OPENFS_JOURNAL_HEADER_SIZE, (size_t)len);
        items[item_count].tx = tx;
        items[item_count].len = len;
        items[item_count].offset = payload_size;
        item_count++;
        payload_size = needed;
    }

    /* No callback can run until every replay input has been staged. */
    for (size_t i = 0U; i < item_count; i++) {
        result = cb(ctx, items[i].tx,
                    payloads + items[i].offset, items[i].len);
        if (result != OPENFS_JOURNAL_OK) goto cleanup;
    }
    if (d->flush(d->context) != OPENFS_IO_OK)
        result = OPENFS_JOURNAL_IO_ERROR;

cleanup:
    free(payloads);
    free(items);
    free(fingerprints);
    free(txids);
    free(states);
    free(b);
    return result;
}
static openfs_journal_result_t journal_write_block_unlocked(openfs_journal_t*j,openfs_block_device_t*d,uint64_t tx,uint64_t target,const void*data){if(j==NULL||!openfs_block_device_is_valid(d)||data==NULL)return OPENFS_JOURNAL_INVALID_ARGUMENT;if(j->recovery_required!=0U||j->publication_in_progress!=0U)return OPENFS_JOURNAL_IO_ERROR;if(j->block_size!=d->block_size||j->journal_blocks==0U||j->active_transaction_id==0U||tx!=j->active_transaction_id)return OPENFS_JOURNAL_INVALID_ARGUMENT;if(d->block_size<=OPENFS_JOURNAL_HEADER_SIZE+OPENFS_JOURNAL_BLOCK_DATA_HEADER)return OPENFS_JOURNAL_INVALID_ARGUMENT;uint64_t capacity=(uint64_t)d->block_size-OPENFS_JOURNAL_HEADER_SIZE-OPENFS_JOURNAL_BLOCK_DATA_HEADER;uint64_t records=((uint64_t)d->block_size+capacity-1U)/capacity;if(j->next_record>j->journal_blocks||records>j->journal_blocks-j->next_record)return OPENFS_JOURNAL_FULL;if(target>=d->block_count||target==0U||target==d->block_count-1U)return OPENFS_JOURNAL_INVALID_ARGUMENT;if(j->journal_start<d->block_count&&target>=j->journal_start&&target-j->journal_start<j->journal_blocks)return OPENFS_JOURNAL_INVALID_ARGUMENT;uint64_t offset=0U;const uint8_t*src=(const uint8_t*)data;while(offset<(uint64_t)d->block_size){uint64_t remain=(uint64_t)d->block_size-offset;uint32_t chunk=(uint32_t)(remain<capacity?remain:capacity);uint32_t payload_len=OPENFS_JOURNAL_BLOCK_DATA_HEADER+chunk;uint8_t*p=malloc(payload_len);if(p==NULL)return OPENFS_JOURNAL_IO_ERROR;memset(p,0,payload_len);memcpy(p,"OJBD1",5U);p64(p+8U,target);p32(p+16U,(uint32_t)offset);p32(p+20U,chunk);memcpy(p+OPENFS_JOURNAL_BLOCK_DATA_HEADER,src+(size_t)offset,chunk);openfs_journal_result_t r=journal_write_unlocked(j,d,tx,p,payload_len);free(p);if(r!=OPENFS_JOURNAL_OK)return r;offset+=chunk;}return OPENFS_JOURNAL_OK;}

static openfs_journal_result_t journal_checkpoint_unlocked(openfs_journal_t*j,openfs_block_device_t*d){if(j==NULL||!openfs_block_device_is_valid(d))return OPENFS_JOURNAL_INVALID_ARGUMENT;if(j->recovery_required!=0U)return OPENFS_JOURNAL_IO_ERROR;if(j->active_transaction_id!=0U||j->journal_blocks==0U||j->block_size!=d->block_size)return OPENFS_JOURNAL_INVALID_ARGUMENT;if(j->journal_start>=d->block_count||j->journal_blocks>d->block_count-j->journal_start||j->journal_blocks>SIZE_MAX/j->block_size)return OPENFS_JOURNAL_IO_ERROR;size_t total=(size_t)(j->journal_blocks*(uint64_t)j->block_size);uint8_t*backup=malloc(total);uint8_t*z=calloc(1U,d->block_size);if(backup==NULL||z==NULL){free(backup);free(z);return OPENFS_JOURNAL_IO_ERROR;}for(uint64_t n=0U;n<j->journal_blocks;n++){if(d->read(d->context,j->journal_start+n,1U,backup+(size_t)(n*j->block_size))!=OPENFS_IO_OK){free(backup);free(z);return OPENFS_JOURNAL_IO_ERROR;}}int ok=1;for(uint64_t n=j->journal_blocks;n>0U;n--){uint64_t slot=n-1U;if(d->write(d->context,j->journal_start+slot,1U,z)!=OPENFS_IO_OK){ok=0;break;}}if(ok&&d->flush(d->context)!=OPENFS_IO_OK)ok=0;if(!ok){int rollback_ok=1;for(uint64_t n=0U;n<j->journal_blocks;n++)if(d->write(d->context,j->journal_start+n,1U,backup+(size_t)(n*j->block_size))!=OPENFS_IO_OK)rollback_ok=0;if(d->flush(d->context)!=OPENFS_IO_OK)rollback_ok=0;free(backup);free(z);if(!rollback_ok)j->recovery_required=1U;return rollback_ok?OPENFS_JOURNAL_IO_ERROR:OPENFS_JOURNAL_CORRUPT;}free(backup);free(z);j->next_record=0U;if(j->sequence==UINT64_MAX)j->sequence=0U;j->commit_record_written=0U;return OPENFS_JOURNAL_OK;}

static openfs_journal_result_t with_journal_lock(openfs_journal_t*j,openfs_journal_result_t (*fn)(openfs_journal_t*,openfs_block_device_t*,uint64_t),openfs_block_device_t*d,uint64_t tx){
    if(j==NULL)return OPENFS_JOURNAL_INVALID_ARGUMENT;
    if(j->runtime==NULL)return fn(j,d,tx);
    if(!openfs_runtime_enter(j->runtime))return OPENFS_JOURNAL_IO_ERROR;
    if(openfs_mutex_lock(&j->runtime->journal_lock,OPENFS_LOCK_RANK_JOURNAL)!=OPENFS_LOCK_OK){openfs_runtime_leave(j->runtime);return OPENFS_JOURNAL_IO_ERROR;}
    openfs_journal_result_t r=fn(j,d,tx);(void)openfs_mutex_unlock(&j->runtime->journal_lock);openfs_runtime_leave(j->runtime);return r;
}
openfs_journal_result_t openfs_journal_begin(openfs_journal_t*j,openfs_block_device_t*d,uint64_t*tx)
{
    if(j==NULL)return OPENFS_JOURNAL_INVALID_ARGUMENT;
    if(j->runtime==NULL)return journal_begin_unlocked(j,d,tx);
    if(!openfs_runtime_enter(j->runtime))return OPENFS_JOURNAL_IO_ERROR;
    if(openfs_mutex_lock(&j->runtime->journal_lock,OPENFS_LOCK_RANK_JOURNAL)!=OPENFS_LOCK_OK){openfs_runtime_leave(j->runtime);return OPENFS_JOURNAL_IO_ERROR;}
    openfs_journal_result_t r=journal_begin_unlocked(j,d,tx);(void)openfs_mutex_unlock(&j->runtime->journal_lock);openfs_runtime_leave(j->runtime);return r;
}
openfs_journal_result_t openfs_journal_write(openfs_journal_t*j,openfs_block_device_t*d,uint64_t tx,const void*data,uint32_t len)
{
    if(j==NULL)return OPENFS_JOURNAL_INVALID_ARGUMENT;
    if(j->runtime==NULL)return journal_write_unlocked(j,d,tx,data,len);
    if(!openfs_runtime_enter(j->runtime))return OPENFS_JOURNAL_IO_ERROR;
    if(openfs_mutex_lock(&j->runtime->journal_lock,OPENFS_LOCK_RANK_JOURNAL)!=OPENFS_LOCK_OK){openfs_runtime_leave(j->runtime);return OPENFS_JOURNAL_IO_ERROR;}
    openfs_journal_result_t r=journal_write_unlocked(j,d,tx,data,len);(void)openfs_mutex_unlock(&j->runtime->journal_lock);openfs_runtime_leave(j->runtime);return r;
}
openfs_journal_result_t openfs_journal_commit(openfs_journal_t*j,openfs_block_device_t*d,uint64_t tx)
{
    if(j==NULL)return OPENFS_JOURNAL_INVALID_ARGUMENT;
    if(j->runtime==NULL)return journal_commit_unlocked(j,d,tx);
    if(!openfs_runtime_enter(j->runtime))return OPENFS_JOURNAL_IO_ERROR;
    if(openfs_mutex_lock(&j->runtime->journal_lock,OPENFS_LOCK_RANK_JOURNAL)!=OPENFS_LOCK_OK){openfs_runtime_leave(j->runtime);return OPENFS_JOURNAL_IO_ERROR;}
    openfs_journal_result_t r=journal_commit_unlocked(j,d,tx);(void)openfs_mutex_unlock(&j->runtime->journal_lock);openfs_runtime_leave(j->runtime);return r;
}
openfs_journal_result_t openfs_journal_commit_transaction(openfs_journal_t*j,openfs_block_device_t*d,uint64_t tx,int*committed)
{
    if(j==NULL||committed==NULL)return OPENFS_JOURNAL_INVALID_ARGUMENT;
    *committed=0;
    if(j->runtime==NULL){
        uint64_t next_record_before=j->next_record;
        openfs_journal_result_t r=journal_commit_unlocked(j,d,tx);
        if(j->commit_record_written!=0U&&j->next_record>next_record_before){
            *committed=1;
            if(r==OPENFS_JOURNAL_OK)j->publication_in_progress=1U;
            else{j->recovery_required=1U;j->publication_in_progress=0U;}
        }
        return r;
    }
    if(!openfs_runtime_enter(j->runtime))return OPENFS_JOURNAL_IO_ERROR;
    if(openfs_mutex_lock(&j->runtime->journal_lock,OPENFS_LOCK_RANK_JOURNAL)!=OPENFS_LOCK_OK){openfs_runtime_leave(j->runtime);return OPENFS_JOURNAL_IO_ERROR;}
    uint64_t next_record_before=j->next_record;
    openfs_journal_result_t r=journal_commit_unlocked(j,d,tx);
    if(j->commit_record_written!=0U&&j->next_record>next_record_before){
        *committed=1;
        if(r==OPENFS_JOURNAL_OK)j->publication_in_progress=1U;
        else{j->recovery_required=1U;j->publication_in_progress=0U;}
    }
    (void)openfs_mutex_unlock(&j->runtime->journal_lock);
    openfs_runtime_leave(j->runtime);
    return r;
}
openfs_journal_result_t openfs_journal_checkpoint_transaction(openfs_journal_t*j,openfs_block_device_t*d)
{
    if(j==NULL)return OPENFS_JOURNAL_INVALID_ARGUMENT;
    if(j->runtime==NULL){
        if(j->publication_in_progress==0U)return OPENFS_JOURNAL_INVALID_ARGUMENT;
        openfs_journal_result_t r=journal_checkpoint_unlocked(j,d);
        if(r==OPENFS_JOURNAL_OK)j->publication_in_progress=0U;
        else{j->recovery_required=1U;j->publication_in_progress=0U;}
        return r;
    }
    if(!openfs_runtime_enter(j->runtime))return OPENFS_JOURNAL_IO_ERROR;
    if(openfs_mutex_lock(&j->runtime->journal_lock,OPENFS_LOCK_RANK_JOURNAL)!=OPENFS_LOCK_OK){openfs_runtime_leave(j->runtime);return OPENFS_JOURNAL_IO_ERROR;}
    openfs_journal_result_t r;
    if(j->publication_in_progress==0U)r=OPENFS_JOURNAL_INVALID_ARGUMENT;
    else{
        r=journal_checkpoint_unlocked(j,d);
        if(r==OPENFS_JOURNAL_OK)j->publication_in_progress=0U;
        else{j->recovery_required=1U;j->publication_in_progress=0U;}
    }
    (void)openfs_mutex_unlock(&j->runtime->journal_lock);
    openfs_runtime_leave(j->runtime);
    return r;
}
openfs_journal_result_t openfs_journal_mark_recovery_required(openfs_journal_t*j)
{
    if(j==NULL)return OPENFS_JOURNAL_INVALID_ARGUMENT;
    if(j->runtime==NULL){j->recovery_required=1U;j->publication_in_progress=0U;return OPENFS_JOURNAL_OK;}
    if(!openfs_runtime_enter(j->runtime))return OPENFS_JOURNAL_IO_ERROR;
    if(openfs_mutex_lock(&j->runtime->journal_lock,OPENFS_LOCK_RANK_JOURNAL)!=OPENFS_LOCK_OK){openfs_runtime_leave(j->runtime);return OPENFS_JOURNAL_IO_ERROR;}
    j->recovery_required=1U;
    j->publication_in_progress=0U;
    (void)openfs_mutex_unlock(&j->runtime->journal_lock);
    openfs_runtime_leave(j->runtime);
    return OPENFS_JOURNAL_OK;
}
openfs_journal_result_t openfs_journal_recover(openfs_journal_t*j,const openfs_block_device_t*d,const openfs_superblock_t*s,openfs_journal_replay_fn cb,void*ctx)
{
    if(j==NULL||!range(d,s)||cb==NULL)return OPENFS_JOURNAL_INVALID_ARGUMENT;
    if(j->journal_start!=s->journal_start||j->journal_blocks!=s->journal_blocks||j->block_size!=d->block_size||s->block_size!=d->block_size)return OPENFS_JOURNAL_INVALID_ARGUMENT;

    /*
     * Reserve recovery before replay so journal mutations cannot race replay.
     * Keep runtime admission for the entire replay: otherwise shutdown could
     * destroy the runtime after the gate is set but before replay completes.
     */
    if(j->runtime==NULL){
        if(j->active_transaction_id!=0U||j->publication_in_progress!=0U)return OPENFS_JOURNAL_INVALID_ARGUMENT;
        j->recovery_required=1U;
        openfs_journal_result_t result=openfs_journal_replay(d,s,cb,ctx);
        if(result==OPENFS_JOURNAL_OK){j->recovery_required=0U;j->publication_in_progress=0U;}
        return result;
    }
    if(!openfs_runtime_enter(j->runtime))return OPENFS_JOURNAL_IO_ERROR;
    if(openfs_mutex_lock(&j->runtime->journal_lock,OPENFS_LOCK_RANK_JOURNAL)!=OPENFS_LOCK_OK){
        openfs_runtime_leave(j->runtime);
        return OPENFS_JOURNAL_IO_ERROR;
    }
    if(j->active_transaction_id!=0U||j->publication_in_progress!=0U){
        (void)openfs_mutex_unlock(&j->runtime->journal_lock);
        openfs_runtime_leave(j->runtime);
        return OPENFS_JOURNAL_INVALID_ARGUMENT;
    }
    j->recovery_required=1U;
    (void)openfs_mutex_unlock(&j->runtime->journal_lock);

    openfs_journal_result_t result=openfs_journal_replay(d,s,cb,ctx);
    if(result==OPENFS_JOURNAL_OK){
        if(openfs_mutex_lock(&j->runtime->journal_lock,OPENFS_LOCK_RANK_JOURNAL)!=OPENFS_LOCK_OK){
            result=OPENFS_JOURNAL_IO_ERROR;
        }else{
            if(j->active_transaction_id!=0U)result=OPENFS_JOURNAL_INVALID_ARGUMENT;
            else{j->recovery_required=0U;j->publication_in_progress=0U;}
            (void)openfs_mutex_unlock(&j->runtime->journal_lock);
        }
    }
    openfs_runtime_leave(j->runtime);
    return result;
}
openfs_journal_result_t openfs_journal_checkpoint(openfs_journal_t*j,openfs_block_device_t*d)
{
    if(j==NULL)return OPENFS_JOURNAL_INVALID_ARGUMENT;
    if(j->runtime==NULL){
        if(j->publication_in_progress!=0U)return OPENFS_JOURNAL_IO_ERROR;
        return journal_checkpoint_unlocked(j,d);
    }
    if(!openfs_runtime_enter(j->runtime))return OPENFS_JOURNAL_IO_ERROR;
    if(openfs_mutex_lock(&j->runtime->journal_lock,OPENFS_LOCK_RANK_JOURNAL)!=OPENFS_LOCK_OK){openfs_runtime_leave(j->runtime);return OPENFS_JOURNAL_IO_ERROR;}
    openfs_journal_result_t r=j->publication_in_progress!=0U?OPENFS_JOURNAL_IO_ERROR:journal_checkpoint_unlocked(j,d);
    (void)openfs_mutex_unlock(&j->runtime->journal_lock);openfs_runtime_leave(j->runtime);return r;
}
openfs_journal_result_t openfs_journal_write_block(openfs_journal_t*j,openfs_block_device_t*d,uint64_t tx,uint64_t target,const void*data)
{
    if(j==NULL)return OPENFS_JOURNAL_INVALID_ARGUMENT;
    if(j->runtime==NULL)return journal_write_block_unlocked(j,d,tx,target,data);
    if(!openfs_runtime_enter(j->runtime))return OPENFS_JOURNAL_IO_ERROR;
    if(openfs_mutex_lock(&j->runtime->journal_lock,OPENFS_LOCK_RANK_JOURNAL)!=OPENFS_LOCK_OK){openfs_runtime_leave(j->runtime);return OPENFS_JOURNAL_IO_ERROR;}
    openfs_journal_result_t r=journal_write_block_unlocked(j,d,tx,target,data);(void)openfs_mutex_unlock(&j->runtime->journal_lock);openfs_runtime_leave(j->runtime);return r;
}
