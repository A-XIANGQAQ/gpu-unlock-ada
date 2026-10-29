/* SPDX-License-Identifier: GPL-2.0-only
 * Opt-in only. The hardware evidence for this sequence is in
 * docs/windows-ad103-vga-restore.md; it was collected by the separate Windows
 * audit harness, not by this app. The default build does not call this code.
 */
#include "vga_restore.h"
static EFI_GUID PciGuid=EFI_PCI_IO_PROTOCOL_GUID;
/* These are the exact bits observed in the captured pre-Stop attribute value
 * and cleared by the matching owner Stop. They are restored as an observed
 * vendor mask; they are not a claim about another platform's encoding. */
#define GU_VGA_MASK 0x18ULL

static UINT16 Command(const UINT32 c[64]){return (UINT16)(c[1]&0xffffu);}
static UINT16 BridgeControl(const UINT32 c[64]){return (UINT16)((c[15]>>16)&0xffffu);}
static EFI_STATUS ReadState(EFI_PCI_IO_PROTOCOL *io,UINT32 c[64],UINT64 *attrs){
  if(!io||!io->Pci.Read||!io->Attributes||!c||!attrs)return EFI_UNSUPPORTED;
  EFI_STATUS s=io->Pci.Read(io,EfiPciIoWidthUint32,0,64,c);
  if(EFI_ERROR(s))return s;
  return io->Attributes(io,EfiPciIoAttributeOperationGet,0,attrs);
}
/* Find the single PCI-to-PCI bridge whose secondary bus is the GPU's bus. */
static EFI_STATUS FindParent(EFI_PCI_IO_PROTOCOL **parent){
  EFI_BOOT_SERVICES *bs=Gu.bs;EFI_HANDLE *handles=NULL;UINTN count=0,found=0;
  UINTN seg=0,bus=0,dev=0,fun=0;*parent=NULL;
  if(!bs||!bs->LocateHandleBuffer||!bs->HandleProtocol||!bs->FreePool||
     !Gu.pci||!Gu.pci->GetLocation)return EFI_UNSUPPORTED;
  EFI_STATUS s=Gu.pci->GetLocation(Gu.pci,&seg,&bus,&dev,&fun);
  if(EFI_ERROR(s)||seg!=0)return EFI_NOT_READY;
  s=bs->LocateHandleBuffer(ByProtocol,&PciGuid,NULL,&count,&handles);
  if(EFI_ERROR(s)||!handles||!count||count>1024u)s=EFI_NOT_READY;
  if(!EFI_ERROR(s))for(UINTN i=0;i<count;i++){
    EFI_PCI_IO_PROTOCOL *io=NULL;UINT32 c[64];
    if(bs->HandleProtocol(handles[i],&PciGuid,(VOID **)&io)!=EFI_SUCCESS||!io||io==Gu.pci)continue;
    if(!io->Pci.Read||EFI_ERROR(io->Pci.Read(io,EfiPciIoWidthUint32,0,64,c)))continue;
    if((c[2]>>16)!=0x0604u||((c[6]>>8)&0xffu)!=bus)continue;
    *parent=io;found++;
  }
  if(handles)(VOID)bs->FreePool(handles);
  if(found!=1){*parent=NULL;return EFI_NOT_READY;}
  return EFI_SUCCESS;
}

EFI_STATUS GuRestoreVga(VOID){
  EFI_PCI_IO_PROTOCOL *parent=NULL;
  UINT32 gpuCfg[64],parentCfg[64],againGpu[64],againParent[64];
  UINT64 gpuAttrs=0,parentAttrs=0,againGpuAttrs=0,againParentAttrs=0;
  BOOLEAN postFlr=(BOOLEAN)(Gu.started&&Gu.quiesced&&Gu.flrs==1);
  if(!Gu.pci||!Gu.pci->Attributes||!Gu.owned||Gu.allocations||
     Gu.dma_count||Gu.master_attributes_pending||Gu.fault||
     (Gu.started&&!postFlr))return EFI_NOT_READY;
  EFI_STATUS s=FindParent(&parent);
  if(EFI_ERROR(s)){
    GuLog("VGA restore refused: parent bridge not uniquely identified status=%#llx\n",
      (unsigned long long)s);
    return s;
  }
  s=ReadState(Gu.pci,gpuCfg,&gpuAttrs);
  if(!EFI_ERROR(s))s=ReadState(parent,parentCfg,&parentAttrs);
  if(EFI_ERROR(s)){
    GuLog("VGA restore refused: state read status=%#llx\n",(unsigned long long)s);
    return s;
  }
  if(gpuAttrs!=0x300ULL||Command(gpuCfg)!=0x3u||(gpuCfg[1]&4u)){
    GuLog("VGA restore refused: gpu pre-state attr=%#llx cmd=%#x\n",
      (unsigned long long)gpuAttrs,(unsigned)Command(gpuCfg));
    return EFI_NOT_READY;
  }
  if(parentAttrs!=0x700ULL||Command(parentCfg)!=0x7u||BridgeControl(parentCfg)!=0x10u){
    GuLog("VGA restore refused: parent pre-state attr=%#llx cmd=%#x bctl=%#x\n",
      (unsigned long long)parentAttrs,(unsigned)Command(parentCfg),
      (unsigned)BridgeControl(parentCfg));
    return EFI_NOT_READY;
  }
  GuLog("VGA restore before: gpu_attr=%#llx gpu_cmd=%#x p_attr=%#llx p_cmd=%#x p_bctl=%#x\n",
    (unsigned long long)gpuAttrs,(unsigned)Command(gpuCfg),
    (unsigned long long)parentAttrs,(unsigned)Command(parentCfg),
    (unsigned)BridgeControl(parentCfg));
  s=Gu.pci->Attributes(Gu.pci,EfiPciIoAttributeOperationEnable,GU_VGA_MASK,NULL);
  if(EFI_ERROR(s)){
    GuLog("VGA restore GPU enable failed status=%#llx\n",(unsigned long long)s);
    return s;
  }
  s=ReadState(Gu.pci,againGpu,&againGpuAttrs);
  if(!EFI_ERROR(s))s=ReadState(parent,againParent,&againParentAttrs);
  if(EFI_ERROR(s)){
    GuLog("VGA restore readback failed status=%#llx\n",(unsigned long long)s);
    return s;
  }
  if(againGpuAttrs!=0x318ULL||Command(againGpu)!=0x3u||(againGpu[1]&4u)){
    GuLog("VGA restore GPU readback refused: attr=%#llx cmd=%#x\n",
      (unsigned long long)againGpuAttrs,(unsigned)Command(againGpu));
    return EFI_NOT_READY;
  }
  if(againParentAttrs!=0x718ULL||Command(againParent)!=0x7u||
     BridgeControl(againParent)!=0x18u){
    GuLog("VGA restore parent not updated by GPU enable: attr=%#llx bctl=%#x; trying parent enable\n",
      (unsigned long long)againParentAttrs,(unsigned)BridgeControl(againParent));
    s=parent->Attributes(parent,EfiPciIoAttributeOperationEnable,GU_VGA_MASK,NULL);
    if(EFI_ERROR(s)){
      GuLog("VGA restore parent enable failed status=%#llx\n",(unsigned long long)s);
      return s;
    }
    s=ReadState(parent,againParent,&againParentAttrs);
    if(EFI_ERROR(s)){
      GuLog("VGA restore parent readback failed status=%#llx\n",(unsigned long long)s);
      return s;
    }
    if(againParentAttrs!=0x718ULL||Command(againParent)!=0x7u||
       BridgeControl(againParent)!=0x18u){
      GuLog("VGA restore parent readback refused: attr=%#llx cmd=%#x bctl=%#x\n",
        (unsigned long long)againParentAttrs,(unsigned)Command(againParent),
        (unsigned)BridgeControl(againParent));
      return EFI_NOT_READY;
    }
  }
  /* Repeat both reads after the parent path to reject a transient change. */
  UINT64 finalGpu=0,finalParent=0;
  s=ReadState(Gu.pci,againGpu,&finalGpu);
  if(!EFI_ERROR(s))s=ReadState(parent,againParent,&finalParent);
  if(EFI_ERROR(s)||finalGpu!=0x318ULL||finalParent!=0x718ULL||
     Command(againGpu)!=0x3u||Command(againParent)!=0x7u||
     BridgeControl(againParent)!=0x18u){
    GuLog("VGA restore final readback refused: gpu=%#llx parent=%#llx bctl=%#x status=%#llx\n",
      (unsigned long long)finalGpu,(unsigned long long)finalParent,
      (unsigned)BridgeControl(againParent),(unsigned long long)s);
    return EFI_ERROR(s)?s:EFI_NOT_READY;
  }
  GuLog("VGA RESTORE OK: gpu_attr=0x318 gpu_cmd=0x3 p_attr=0x718 p_cmd=0x7 p_bctl=0x18\n");
  return EFI_SUCCESS;
}
