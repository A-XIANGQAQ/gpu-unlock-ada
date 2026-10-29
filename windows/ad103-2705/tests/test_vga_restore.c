/* Host-only tests for the opt-in VGA restore. No firmware or GPU access. */
#pragma warning(disable:4005)
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <Uefi.h>
#include "../port.h"

static char Lines[64][256];static UINTN LineCount;
VOID GuLog(const char *format,...){
    va_list a;va_start(a,format);
    if(LineCount<64)vsnprintf(Lines[LineCount],sizeof(Lines[0]),format,a);
    va_end(a);if(LineCount<64)LineCount++;
}
GU_PLATFORM Gu;
static EFI_PCI_IO_PROTOCOL Gpu,Parent,Other;
static UINT32 GpuCfg[64],ParentCfg[64],OtherCfg[64];
static UINT64 GpuAttrs,ParentAttrs;
static EFI_STATUS GpuEnableStatus,ParentEnableStatus,GpuReadStatus,ParentReadStatus;
static BOOLEAN GpuEnableUpdatesParent,ParentEnableWorks,Wrote;
static UINTN GpuEnables,ParentEnables,Disables,GpuBus;
static EFI_HANDLE Handles[3]={(EFI_HANDLE)(UINTN)1,(EFI_HANDLE)(UINTN)2,(EFI_HANDLE)(UINTN)3};
static EFI_PCI_IO_PROTOCOL *ProtocolFor(EFI_HANDLE h){
    if(h==Handles[0])return &Gpu;if(h==Handles[1])return &Parent;if(h==Handles[2])return &Other;return NULL;
}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *g,VOID *search,UINTN *n,EFI_HANDLE **out){
    (VOID)type;(VOID)g;(VOID)search;if(!n||!out)return EFI_INVALID_PARAMETER;*n=3;*out=Handles;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE h,EFI_GUID *g,VOID **out){
    (VOID)g;if(!out)return EFI_INVALID_PARAMETER;*out=ProtocolFor(h);return *out?EFI_SUCCESS:EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI Free(VOID *p){(VOID)p;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Attributes(EFI_PCI_IO_PROTOCOL *io,EFI_PCI_IO_PROTOCOL_ATTRIBUTE_OPERATION op,UINT64 mask,UINT64 *out){
    if(op==EfiPciIoAttributeOperationGet){
        if(!out)return EFI_INVALID_PARAMETER;
        *out=io==&Gpu?GpuAttrs:ParentAttrs;return EFI_SUCCESS;
    }
    if(op==EfiPciIoAttributeOperationSupported){if(!out)return EFI_INVALID_PARAMETER;*out=0x1377b;return EFI_SUCCESS;}
    if(op==EfiPciIoAttributeOperationEnable){
        if(io==&Gpu){GpuEnables++;if(EFI_ERROR(GpuEnableStatus))return GpuEnableStatus;GpuAttrs|=mask;
            if(GpuEnableUpdatesParent){ParentAttrs|=mask;ParentCfg[15]|=0x00080000;}}
        else {ParentEnables++;if(EFI_ERROR(ParentEnableStatus))return ParentEnableStatus;
            if(ParentEnableWorks){ParentAttrs|=mask;ParentCfg[15]|=0x00080000;}}
        return EFI_SUCCESS;
    }
    if(op==EfiPciIoAttributeOperationDisable){Disables++;return EFI_UNSUPPORTED;}
    return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI Read(EFI_PCI_IO_PROTOCOL *io,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 off,UINTN n,VOID *v){
    (VOID)w;if(!v||off!=0||n!=64)return EFI_INVALID_PARAMETER;
    if(io==&Gpu){if(EFI_ERROR(GpuReadStatus))return GpuReadStatus;memcpy(v,GpuCfg,256);}
    else if(io==&Parent){if(EFI_ERROR(ParentReadStatus))return ParentReadStatus;memcpy(v,ParentCfg,256);}
    else memcpy(v,OtherCfg,256);
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Location(EFI_PCI_IO_PROTOCOL *io,UINTN *s,UINTN *b,UINTN *d,UINTN *f){
    (VOID)io;*s=0;*b=io==&Gpu?GpuBus:(io==&Parent?0:8);*d=io==&Gpu?0:3;*f=io==&Gpu?0:1;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Write(EFI_PCI_IO_PROTOCOL *io,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 off,UINTN n,VOID *v){
    (VOID)io;(VOID)w;(VOID)off;(VOID)n;(VOID)v;Wrote=TRUE;return EFI_UNSUPPORTED;
}
#include "../vga_restore.c"

static UINTN Checks;
static BOOLEAN Has(const char *s){for(UINTN i=0;i<LineCount;i++)if(strstr(Lines[i],s))return TRUE;return FALSE;}
#define CHECK(x) do{Checks++;if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static VOID Reset(VOID){
    LineCount=0;Wrote=FALSE;GpuEnables=ParentEnables=Disables=0;GpuBus=9;
    GpuEnableStatus=ParentEnableStatus=GpuReadStatus=ParentReadStatus=EFI_SUCCESS;
    GpuEnableUpdatesParent=FALSE;ParentEnableWorks=TRUE;GpuAttrs=0x300;ParentAttrs=0x700;
    for(UINTN i=0;i<64;i++){GpuCfg[i]=ParentCfg[i]=OtherCfg[i]=0;}
    GpuCfg[1]=0x00000003;ParentCfg[1]=0x00000007;ParentCfg[2]=0x06040000;ParentCfg[6]=0x00090900;ParentCfg[15]=0x00100000;
    OtherCfg[2]=0x06040000;OtherCfg[6]=0x00080800;
    memset(&Gpu,0,sizeof(Gpu));memset(&Parent,0,sizeof(Parent));memset(&Other,0,sizeof(Other));
    Gpu.Attributes=Attributes;Gpu.Pci.Read=Read;Gpu.Pci.Write=Write;Gpu.GetLocation=Location;
    Parent.Attributes=Attributes;Parent.Pci.Read=Read;Parent.Pci.Write=Write;Parent.GetLocation=Location;
    Other.Attributes=Attributes;Other.Pci.Read=Read;Other.Pci.Write=Write;Other.GetLocation=Location;
    memset(&Gu,0,sizeof(Gu));Gu.bs=(EFI_BOOT_SERVICES *)1;Gu.pci=&Gpu;Gu.owned=TRUE;
}
int main(VOID){
    EFI_BOOT_SERVICES bs;memset(&bs,0,sizeof(bs));
    bs.LocateHandleBuffer=Locate;bs.HandleProtocol=Handle;bs.FreePool=Free;
    Reset();Gu.bs=&bs;GpuEnableUpdatesParent=TRUE;
    CHECK(GuRestoreVga()==EFI_SUCCESS);
    CHECK(GpuAttrs==0x318&&ParentAttrs==0x718&&((ParentCfg[15]>>16)&0xffff)==0x18);
    CHECK(GpuEnables==1&&ParentEnables==0&&Disables==0&&!Wrote&&Has("VGA RESTORE OK"));

    Reset();Gu.bs=&bs;GpuEnableUpdatesParent=FALSE;ParentEnableWorks=TRUE;
    CHECK(GuRestoreVga()==EFI_SUCCESS&&GpuAttrs==0x318&&ParentAttrs==0x718&&GpuEnables==1&&ParentEnables==1);

    Reset();Gu.bs=&bs;GpuAttrs=0x700;
    CHECK(EFI_ERROR(GuRestoreVga())&&GpuEnables==0);

    Reset();Gu.bs=&bs;GpuCfg[1]=0x00000007;
    CHECK(EFI_ERROR(GuRestoreVga())&&GpuEnables==0);

    Reset();Gu.bs=&bs;ParentAttrs=0x300;
    CHECK(EFI_ERROR(GuRestoreVga())&&GpuEnables==0);

    Reset();Gu.bs=&bs;ParentCfg[15]=0x00080000;
    CHECK(EFI_ERROR(GuRestoreVga())&&GpuEnables==0);

    Reset();Gu.bs=&bs;OtherCfg[6]=0x00090900; /* Two bridges claim secondary bus 9. */
    CHECK(EFI_ERROR(GuRestoreVga())&&GpuEnables==0&&Has("parent bridge"));

    Reset();Gu.bs=&bs;GpuEnableStatus=EFI_DEVICE_ERROR;
    CHECK(EFI_ERROR(GuRestoreVga())&&GpuEnables==1&&ParentEnables==0);

    Reset();Gu.bs=&bs;GpuReadStatus=EFI_DEVICE_ERROR;
    CHECK(EFI_ERROR(GuRestoreVga())&&GpuEnables==0);

    Reset();Gu.bs=&bs;GpuEnableUpdatesParent=FALSE;ParentEnableWorks=FALSE;
    CHECK(EFI_ERROR(GuRestoreVga())&&ParentEnables==1&&Has("parent readback refused"));

    Reset();Gu.bs=&bs;GpuEnableUpdatesParent=FALSE;ParentEnableStatus=EFI_DEVICE_ERROR;
    CHECK(EFI_ERROR(GuRestoreVga())&&ParentEnables==1&&Has("parent enable failed"));

    Reset();Gu.bs=&bs;GpuEnableUpdatesParent=TRUE;Gu.started=TRUE;Gu.quiesced=TRUE;Gu.flrs=1;
    CHECK(GuRestoreVga()==EFI_SUCCESS&&GpuEnables==1);

    Reset();Gu.bs=&bs;Gu.started=TRUE;Gu.quiesced=FALSE;Gu.flrs=1;
    CHECK(EFI_ERROR(GuRestoreVga())&&GpuEnables==0);

    Reset();Gu.bs=&bs;Gu.started=TRUE;Gu.quiesced=TRUE;Gu.flrs=0;
    CHECK(EFI_ERROR(GuRestoreVga())&&GpuEnables==0);

    CHECK(Disables==0&&!Wrote);
    printf("PASS: %u VGA-restore checks; no firmware or GPU access.\n",(unsigned)Checks);
    return 0;
}
