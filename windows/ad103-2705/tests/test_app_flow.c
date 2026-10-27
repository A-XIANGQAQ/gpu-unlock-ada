/* SPDX-License-Identifier: GPL-2.0-only */
/* Real application entry; dependency effects simulated and order asserted. */
#include "../app.c"
#include <stdio.h>
#include <stdlib.h>
#include <setjmp.h>
#include <stdarg.h>
#include <string.h>
static unsigned Checks,Case,Calls,Releases,Transactions,Finishes,WindowsCalls,Closes;
static BOOLEAN Mutated,Silenced,Cold,Unheld;static unsigned Scenarios;static jmp_buf Escape;
static EFI_PCI_IO_PROTOCOL Io;static EFI_BOOT_SERVICES Bs;static EFI_RUNTIME_SERVICES Rs;static EFI_SYSTEM_TABLE St;
GU_PLATFORM Gu;
#define CHECK(x) do{Checks++;if(!(x)){fprintf(stderr,"app FAIL case=%u line=%d %s\n",Case,__LINE__,#x);exit(1);}}while(0)
static EFI_STATUS Tick(unsigned n){CHECK(++Calls==n);return Case==n?EFI_DEVICE_ERROR:EFI_SUCCESS;}
VOID GuInit(EFI_BOOT_SERVICES *b,EFI_PCI_IO_PROTOCOL *p){memset(&Gu,0,sizeof(Gu));Gu.bs=b;Gu.pci=p;}
/* The terminal stage label is the only thing a person at the screen has, so
 * the fixture keeps the last line instead of dropping it. */
static char LastLog[512];
VOID GuLog(const char *f,...){va_list a;va_start(a,f);vsnprintf(LastLog,sizeof(LastLog),f,a);va_end(a);}
VOID GuLogText(const char *f){(void)f;}
BOOLEAN GuCheckConfig(const UINT32 c[64],UINTN s,UINTN b,UINTN d,UINTN f,GU_CAPS *caps){CHECK(c[0]==GU_BOARD_ID&&!s&&b==8&&!d&&!f);*caps=(GU_CAPS){0x60,0x78,0x68,0,2};return TRUE;}
static EFI_STATUS EFIAPI GetVariable(CHAR16 *name,EFI_GUID *guid,UINT32 *attrs,UINTN *n,VOID *v){(void)name;(void)guid;*attrs=0;*n=1;*(UINT8 *)v=Case==99?1:0;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE kind,EFI_GUID *guid,VOID *key,UINTN *n,EFI_HANDLE **out){(void)kind;(void)guid;(void)key;*n=1;*out=malloc(sizeof(**out));CHECK(*out);(*out)[0]=(EFI_HANDLE)2;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE h,EFI_GUID *guid,VOID **p){(void)guid;CHECK(h==(EFI_HANDLE)2);*p=&Io;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Free(VOID *p){free(p);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Read(EFI_PCI_IO_PROTOCOL *p,EFI_PCI_IO_PROTOCOL_WIDTH w,UINT32 off,UINTN n,VOID *out){CHECK(p==&Io&&w==EfiPciIoWidthUint32&&!off&&(n==1||n==64));memset(out,0,n*4);((UINT32 *)out)[0]=GU_BOARD_ID;if(n==64)((UINT32 *)out)[2]=0x030000a1;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Location(EFI_PCI_IO_PROTOCOL *p,UINTN *s,UINTN *b,UINTN *d,UINTN *f){(void)p;*s=*d=*f=0;*b=8;return EFI_SUCCESS;}
EFI_STATUS GuBootOpen(EFI_HANDLE i,EFI_SYSTEM_TABLE *t){CHECK(i==(EFI_HANDLE)1&&t==&St);return Tick(1);}
EFI_STATUS GuBootLoadAssets(VOID){return Tick(2);}
EFI_STATUS GuAd104Prepare(VOID){EFI_STATUS s=Tick(3);if(s==EFI_SUCCESS)Gu.profile_verified=Gu.prom_verified=TRUE;return s;}
EFI_STATUS GuBootLoadWindows(VOID){return Tick(4);}
EFI_STATUS GuBootChoose(BOOLEAN *u){EFI_STATUS s=Tick(5);*u=Case!=0;return s;}
EFI_STATUS GuMsiPreflight(EFI_SYSTEM_TABLE *t,EFI_HANDLE self,EFI_HANDLE pci){CHECK(t==&St&&self==(EFI_HANDLE)1&&pci==(EFI_HANDLE)2&&Gu.profile_verified);return Tick(6);}
VOID GuBootSay(const CHAR16 *s){(void)s;CHECK(!Silenced);}
VOID GuBootSilence(VOID){CHECK(Calls==6);Silenced=TRUE;}
EFI_STATUS GuMsiRelease(VOID){CHECK(Silenced);Releases++;Mutated=TRUE;Gu.owned=TRUE;return Tick(7);}
/* A release that did not actually take the claim has its own label now, so the
 * fixture can force that state instead of only asserting the gate refusal. */
BOOLEAN GuMsiReleased(VOID){return Mutated&&Gu.owned&&!Unheld;}
/* app.c asks twice: once through GuAd104Prepare before the driver lets go,
 * and again after. This stub is the post-release one, so a case can refuse it. */
BOOLEAN GuAd104ColdEntry(VOID){return Cold;}
int gpu_unlock_610_load_shared(struct pci_dev *d,struct gpu_unlock_610_shared *shared){CHECK(d->device==0x2705&&Gu.owned&&Gu.armed&&Gu.display_ok);(void)shared;return Tick(8)==EFI_SUCCESS?0:-EIO;}
int gpu_unlock_610_unlock(struct pci_dev *d,const struct gpu_unlock_610_shared *s){(void)d;(void)s;Transactions++;Gu.started=TRUE;Gu.quiesced=TRUE;Gu.flrs=1;return Tick(9)==EFI_SUCCESS?0:-EIO;}
void gpu_unlock_610_release_shared(struct gpu_unlock_610_shared *s){(void)s;}
BOOLEAN GuSafeToReturn(VOID){return TRUE;}
EFI_STATUS GuMsiFinish(VOID){CHECK(Transactions&&Gu.quiesced);Finishes++;EFI_STATUS s=Tick(10);if(s==EFI_SUCCESS)Gu.owned=FALSE;return s;}
EFI_STATUS GuClearMaster(VOID){CHECK(Mutated);return EFI_SUCCESS;}
BOOLEAN GuMsiMutated(VOID){return Mutated;}
/* The handoff check runs *before* the log is closed so that a refusal is
 * recorded; a stop after the close used to leave no trace at all. */
BOOLEAN GuMsiReadyForWindows(VOID){CHECK(Closes==0&&!Gu.owned&&Gu.quiesced);return Case!=12;}
EFI_STATUS GuBootClose(VOID){Closes++;return Case==11?EFI_DEVICE_ERROR:EFI_SUCCESS;}
EFI_STATUS GuBootWindows(VOID){WindowsCalls++;if(Case)CHECK(Calls==10&&!Gu.owned&&Transactions==1&&Finishes==1);else CHECK(!Releases&&!Transactions&&!Finishes);return EFI_ABORTED;}
VOID GuBootHalt(VOID){CHECK(Mutated);longjmp(Escape,1);}
int main(void){
  for(volatile unsigned iteration=0;iteration<14;iteration++) {
    Case=iteration==13?99:iteration;Checks++;Calls=Releases=Transactions=Finishes=WindowsCalls=Closes=0;Mutated=Silenced=Unheld=FALSE;Cold=TRUE;
    memset(&Gu,0,sizeof(Gu));memset(&Bs,0,sizeof(Bs));memset(&Rs,0,sizeof(Rs));memset(&St,0,sizeof(St));memset(&Io,0,sizeof(Io));
    Bs.LocateHandleBuffer=Locate;Bs.HandleProtocol=Handle;Bs.FreePool=Free;Rs.GetVariable=GetVariable;St.BootServices=&Bs;St.RuntimeServices=&Rs;Io.Pci.Read=Read;Io.GetLocation=Location;
    Scenarios++;if(!setjmp(Escape))(VOID)UefiMain((EFI_HANDLE)1,&St);
    if(Case==0)CHECK(WindowsCalls==1&&!Mutated);
    else if(Case==99)CHECK(!Calls&&!WindowsCalls);
    else if(Case<=12)CHECK(!WindowsCalls);
    CHECK(Releases<=1&&Transactions<=1&&Finishes<=1&&WindowsCalls<=1);
    printf("app flow case %u PASS\n",Case);
  }
  /* The gate runs again after GuMsiRelease. If it refuses there, the drivers are
   * already detached, so the one thing that must not happen is starting Windows
   * on top of that state; app.c halts instead and says so on screen. */
  Case=13;Calls=Releases=Transactions=Finishes=WindowsCalls=Closes=0;Mutated=Silenced=Unheld=FALSE;Cold=FALSE;
  memset(&Gu,0,sizeof(Gu));
  Scenarios++;if(!setjmp(Escape))(VOID)UefiMain((EFI_HANDLE)1,&St);
  CHECK(Releases==1&&Transactions==0&&Finishes==0&&WindowsCalls==0&&Closes==1);
  CHECK(!strcmp(LastLog,"STOP: stage=post-release-gate status=0x8000000000000006"
    " mutated=1 retained=0 fault=0\n"));
  printf("app flow case %u PASS\n",Case);

  /* Full U path with returned Windows image must stop, never retry. */
  Case=100;Calls=Releases=Transactions=Finishes=WindowsCalls=Closes=0;Mutated=Silenced=Unheld=FALSE;Cold=TRUE;
  Scenarios++;if(!setjmp(Escape))(VOID)UefiMain((EFI_HANDLE)1,&St);
  CHECK(WindowsCalls==1&&Releases==1&&Transactions==1&&Finishes==1&&Closes==1);
  CHECK(!strncmp(LastLog,"TRANSACTION OK: ",16));
  /* The other post-release refusal: the claim was not held. Same place, same
   * decision not to start Windows, different reason on screen. */
  Case=14;Calls=Releases=Transactions=Finishes=WindowsCalls=Closes=0;Mutated=Silenced=FALSE;Cold=TRUE;Unheld=TRUE;
  Scenarios++;if(!setjmp(Escape))(VOID)UefiMain((EFI_HANDLE)1,&St);
  CHECK(Releases==1&&Transactions==0&&Finishes==0&&WindowsCalls==0&&Closes==1);
  CHECK(!strcmp(LastLog,"STOP: stage=release-not-held status=0x8000000000000006"
    " mutated=1 retained=0 fault=0\n"));
  printf("app flow case %u PASS\n",Case);
  printf("app flow: %u checks; %u scenarios, all effects simulated.\n",Checks,Scenarios);return 0;
}
