#include <stdio.h>
#include <string.h>
#include "OpenFSInventory.h"

static unsigned int checks, failures;
static void Check(int ok, const char* name) {
    ++checks;
    if (!ok) { ++failures; fprintf(stderr, "FAIL: %s\n", name); }
}
static void Put16(UCHAR* p, USHORT v) { p[0]=(UCHAR)v; p[1]=(UCHAR)(v>>8); }
static void Put32(UCHAR* p, ULONG v) {
    p[0]=(UCHAR)v; p[1]=(UCHAR)(v>>8); p[2]=(UCHAR)(v>>16); p[3]=(UCHAR)(v>>24);
}
static void Put64(UCHAR* p, ULONGLONG v) {
    ULONG i; for (i=0;i<8;++i) p[i]=(UCHAR)(v>>(i*8));
}
static void Seal(UCHAR* b, SIZE_T size) {
    memset(b+40,0,4);
    Put32(b+40, OpenFsCrc32C(b,size));
}
static void Make(UCHAR* b, ULONG size, ULONG metadataCount, ULONG directoryCount) {
    memset(b,0,size);
    memcpy(b,"OFSMINV\0",8);
    Put16(b+8,1); Put16(b+10,48);
    Put64(b+12,1);
    Put32(b+28,metadataCount); Put32(b+32,directoryCount);
    if (metadataCount) Put64(b+48, 100);
    if (directoryCount) {\n        ULONG offset = 48 + ((metadataCount*80u + size-1)/size)*8u;\n        Put64(b+offset, 101);\n        if (((directoryCount*128u + size-1)/size) > 1) Put64(b+offset+8, 102);\n    }
    Seal(b,size);
}
int main(void) {
    UCHAR bytes[4096];
    OPENFS_INVENTORY_DESCRIPTOR d;
    Make(bytes,sizeof(bytes),40,40);
    Check(OpenFsDecodeInventoryDescriptor(bytes,sizeof(bytes),&d)==STATUS_SUCCESS,
        "valid inventory descriptor decodes");
    Check(d.RootIdLow==1 && d.RootIdHigh==0 && d.MetadataRecordCount==40 &&
        d.DirectoryEntryCount==40, "root ID and record counts decoded");
    Check(d.MetadataBlockCount==1 && d.DirectoryBlockCount==2,
        "block counts use packed record byte sizes");
    Check(OpenFsInventoryDescriptorReadBlock(d.MetadataBlockList)==100 &&
        OpenFsInventoryDescriptorReadBlock(d.DirectoryBlockList)==101,
        "metadata and directory block lists located");
    Check(OpenFsValidateInventoryDescriptorRanges(&d,1000,20,999)==STATUS_SUCCESS,
        "inventory data blocks pass volume range validation");

    bytes[48] ^= 1;
    Check(OpenFsDecodeInventoryDescriptor(bytes,sizeof(bytes),&d)==STATUS_CRC_ERROR,
        "descriptor checksum corruption rejected");

    Make(bytes,sizeof(bytes),40,40);
    bytes[36]=1; Seal(bytes,sizeof(bytes));
    Check(OpenFsDecodeInventoryDescriptor(bytes,sizeof(bytes),&d)==STATUS_DISK_CORRUPT_ERROR,
        "reserved header bytes rejected");

    Make(bytes,sizeof(bytes),40,40);
    bytes[4000]=1; Seal(bytes,sizeof(bytes));
    Check(OpenFsDecodeInventoryDescriptor(bytes,sizeof(bytes),&d)==STATUS_DISK_CORRUPT_ERROR,
        "descriptor padding corruption rejected");

    Make(bytes,sizeof(bytes),40,40);
    Put64(bytes+12,0); Seal(bytes,sizeof(bytes));
    Check(OpenFsDecodeInventoryDescriptor(bytes,sizeof(bytes),&d)==STATUS_DISK_CORRUPT_ERROR,
        "zero root object ID rejected");

    Make(bytes,sizeof(bytes),40,40);
    Put16(bytes+10,47); Seal(bytes,sizeof(bytes));
    Check(OpenFsDecodeInventoryDescriptor(bytes,sizeof(bytes),&d)==STATUS_DISK_CORRUPT_ERROR,
        "invalid descriptor header length rejected");

    Make(bytes,sizeof(bytes),0xFFFFFFFFu,0);
    Check(OpenFsDecodeInventoryDescriptor(bytes,sizeof(bytes),&d)==STATUS_DISK_CORRUPT_ERROR,
        "overflowing metadata record count rejected");

    Make(bytes,sizeof(bytes),40,40);
    Put64(bytes+48,19); Seal(bytes,sizeof(bytes));
    Check(OpenFsDecodeInventoryDescriptor(bytes,sizeof(bytes),&d)==STATUS_SUCCESS,
        "structural decoder leaves physical range validation separate");
    Check(OpenFsValidateInventoryDescriptorRanges(&d,1000,20,999)==STATUS_DISK_CORRUPT_ERROR,
        "metadata block below data region rejected");

    Make(bytes,sizeof(bytes),40,40);
    Put64(bytes+48,100); Put64(bytes+56,100); Seal(bytes,sizeof(bytes));
    Check(OpenFsDecodeInventoryDescriptor(bytes,sizeof(bytes),&d)==STATUS_SUCCESS,
        "duplicate list remains structurally decodable");
    Check(OpenFsValidateInventoryDescriptorRanges(&d,1000,20,999)==STATUS_DISK_CORRUPT_ERROR,
        "duplicate metadata and directory block rejected");

    Make(bytes,sizeof(bytes),40,40);
    Put64(bytes+48,100); Put64(bytes+56,101); Put64(bytes+64,100); Seal(bytes,sizeof(bytes));
    Check(OpenFsDecodeInventoryDescriptor(bytes,sizeof(bytes),&d)==STATUS_SUCCESS,
        "three inventory blocks decode");
    Check(OpenFsValidateInventoryDescriptorRanges(&d,1000,20,999)==STATUS_DISK_CORRUPT_ERROR,
        "duplicate within directory block list rejected");

    Make(bytes,sizeof(bytes),40,40);
    Check(OpenFsDecodeInventoryDescriptor(bytes,2048,&d)==STATUS_INVALID_PARAMETER,
        "undersized descriptor block rejected");
    Check(OpenFsDecodeInventoryDescriptor(NULL,sizeof(bytes),&d)==STATUS_INVALID_PARAMETER,
        "null descriptor bytes rejected");
    Check(OpenFsDecodeInventoryDescriptor(bytes,sizeof(bytes),NULL)==STATUS_INVALID_PARAMETER,
        "null output rejected");

    Make(bytes,sizeof(bytes),0,0);
    Check(OpenFsDecodeInventoryDescriptor(bytes,sizeof(bytes),&d)==STATUS_SUCCESS,
        "empty inventory lists decode");
    Check(OpenFsValidateInventoryDescriptorRanges(&d,1000,20,999)==STATUS_SUCCESS,
        "empty inventory has no invalid block references");

    printf("OpenFS inventory descriptor tests: %u checks, %u failures\n",checks,failures);
    return failures==0?0:1;
}
