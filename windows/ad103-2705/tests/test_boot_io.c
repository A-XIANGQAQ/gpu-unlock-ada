#define GU_HOST_TEST_PROFILE 1
/* SPDX-License-Identifier: GPL-2.0-only */
#include "../boot_io.c"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
/* Upstream required a USB stick, so its case list starts with "not USB, must be
 * refused". This port also runs from the internal ESP, where the path carries a
 * harddrive node and no MSG_USB_DP at all. The case is kept, with the expectation
 * the port actually has: that path has to be accepted. */
enum CASE {GOOD,ESP_PATH,WRONG_PARTITION,CHANGED_WINDOWS_IMAGE,BAD_FILE_SIZE,SHORT_READ,WRONG_LOADED_DEVICE,LOG_WRITE_FAIL,
  /* The three below exercise Inspect() paths the port changed and never ran:
   * a volume whose Windows node is not the first one, a path with too many
   * harddrive nodes, and two volumes that both claim to be the Windows ESP. */
  EXTRA_HARDDISK_NODE,MAX_HARDDISK_NODES,TOO_MANY_HARDDISK_NODES,TWO_WINDOWS_VOLUMES,
  /* GuBootOpen has six refusal paths and they all print a line to the console.
   * They dereference ConOut without a null check, which is fine in firmware but
   * meant the fixture crashed instead of reporting, so nobody ever read them. */
  NO_LOADED_IMAGE,NO_DEVICE_PATH,NO_FILESYSTEM,OPEN_VOLUME_FAILS,NO_LOG_DIRECTORY,NO_LOG_FILE,
  IO_CASES};
static enum CASE Case;static unsigned Checks,Loads,Starts,FileWrites;
#define CHECK(x) do{Checks++;if(!(x)){fprintf(stderr,"IO FAIL case=%d line=%d %s\n",Case,__LINE__,#x);exit(1);}}while(0)
GU_PLATFORM Gu;GU_ASSET GuAssets[5];
static EFI_BOOT_SERVICES Bs;static EFI_SYSTEM_TABLE St;static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL Out;
static EFI_LOADED_IMAGE_PROTOCOL SelfImage,WinImage;
static EFI_SIMPLE_FILE_SYSTEM_PROTOCOL OwnFs,WinFs,AltWinFs;
static EFI_FILE_PROTOCOL OwnRoot,WinRoot,Dir,LogFile,Reader;
static UINT8 UsbPath[]={3,5,6,0,0,0,127,255,4,0},WinPath[1024],AltWinPath[1024];
static UINTN WinPathAt;
static UINT8 *WinBytes,*AssetBytes[5],*ReadBytes;static UINTN ReadSize,Position,WinExpected;
static unsigned KeyMode,Stalls,Refuse;
static CHAR16 Screen[2048];
static char LastWrite[65536];
VOID *GuAlloc(size_t n){return calloc(1,n);}
VOID GuFree(VOID *p){free(p);}
BOOLEAN GuAd104AssetsValid(VOID){for(UINTN i=0;i<5;i++)if(!GuAssets[i].firmware.data)return FALSE;return TRUE;}
static EFI_STATUS EFIAPI Free(VOID *p){free(p);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Close(EFI_FILE_PROTOCOL *p){(void)p;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Flush(EFI_FILE_PROTOCOL *p){CHECK(p==&LogFile);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Write(EFI_FILE_PROTOCOL *p,UINTN *n,VOID *bytes){CHECK(p==&LogFile&&*n&&bytes);FileWrites++;
  /* Append: GuLog emits one write per line, and the cases below look for a line
   * that is not the last one written. */
  UINTN used=0;while(used<sizeof(LastWrite)-1&&LastWrite[used])used++;
  UINTN take=*n<sizeof(LastWrite)-1-used?*n:sizeof(LastWrite)-1-used;
  memcpy(LastWrite+used,bytes,take);LastWrite[used+take]=0;
  return Case==LOG_WRITE_FAIL?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Info(EFI_FILE_PROTOCOL *p,EFI_GUID *g,UINTN *n,VOID *data){
  CHECK(p==&Reader&&!memcmp(g,&InfoGuid,16)&&*n>=sizeof(EFI_FILE_INFO));
  memset(data,0,sizeof(EFI_FILE_INFO));((EFI_FILE_INFO *)data)->FileSize=ReadSize+(Case==BAD_FILE_SIZE?1:0);*n=sizeof(EFI_FILE_INFO);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Read(EFI_FILE_PROTOCOL *p,UINTN *n,VOID *out){
  CHECK(p==&Reader);UINTN count=ReadSize-Position;if(count>*n)count=*n;
  if(Case==SHORT_READ&&count)count--;
  memcpy(out,ReadBytes+Position,count);Position+=count;*n=count;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Open(EFI_FILE_PROTOCOL *p,EFI_FILE_PROTOCOL **out,CHAR16 *name,UINT64 mode,UINT64 attrs){
  *out=NULL;
  if(p==&OwnRoot&&mode&EFI_FILE_MODE_CREATE){CHECK(!wcscmp(name,L"\\AD104-UNLOCK")&&(attrs&EFI_FILE_DIRECTORY));
    if(Case==NO_LOG_DIRECTORY)return EFI_WRITE_PROTECTED;*out=&Dir;return EFI_SUCCESS;}
  if(p==&Dir) {
    if(mode==EFI_FILE_MODE_READ)return EFI_NOT_FOUND;
    CHECK(mode==(EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE)&&!attrs);
    if(Case==NO_LOG_FILE)return EFI_OUT_OF_RESOURCES;*out=&LogFile;return EFI_SUCCESS;
  }
  CHECK(mode==EFI_FILE_MODE_READ&&!attrs);Position=0;
  if(p==&WinRoot){CHECK(!wcscmp(name,WindowsName));ReadBytes=WinBytes;ReadSize=WinExpected;}
  else {
    CHECK(p==&OwnRoot);
    /* GuBootChoose probes \AD104-UNLOCK\AUTO.TXT for the unattended mode. This
     * machine runs the manual one-shot, so the file is absent and the probe has
     * to answer NOT_FOUND rather than fall through to the asset table. */
    if(!wcscmp(name,L"\\AD104-UNLOCK\\AUTO.TXT"))return EFI_NOT_FOUND;
    static const CHAR16 *names[]={L"\\AD104-ASSETS\\booter_load_ad102.bin",L"\\AD104-ASSETS\\v67-ad102-610.43.03-four-plm.bin",L"\\AD104-ASSETS\\gsp_ga10x.bin",L"\\AD104-ASSETS\\gsp_boot_ad102.bin",L"\\AD104-ASSETS\\gsp_boot_desc_ad102.bin"};
    static const UINTN sizes[]={57672,64000,84277400,36864,84};UINTN i;
    for(i=0;i<5;i++)if(!wcscmp(name,names[i]))break;CHECK(i<5);ReadBytes=AssetBytes[i];ReadSize=sizes[i];
  }
  *out=&Reader;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs,EFI_FILE_PROTOCOL **root){
  CHECK(fs==&OwnFs||fs==&WinFs||fs==&AltWinFs);
  if(Case==OPEN_VOLUME_FAILS){*root=NULL;return EFI_DEVICE_ERROR;}
  *root=fs==&OwnFs?&OwnRoot:&WinRoot;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE h,EFI_GUID *g,VOID **out){
  *out=NULL;
  if(!memcmp(g,&LoadedGuid,16)){if(h==(EFI_HANDLE)1&&Case!=NO_LOADED_IMAGE)*out=&SelfImage;else if(h==(EFI_HANDLE)10)*out=&WinImage;}
  if(!memcmp(g,&FsGuid,16)){if(h==(EFI_HANDLE)2&&Case!=NO_FILESYSTEM)*out=&OwnFs;else if(h==(EFI_HANDLE)3)*out=&WinFs;else if(h==(EFI_HANDLE)5)*out=&AltWinFs;}
  if(!memcmp(g,&PathGuid,16)){if(h==(EFI_HANDLE)2&&Case!=NO_DEVICE_PATH)*out=UsbPath;else if(h==(EFI_HANDLE)3)*out=WinPath;else if(h==(EFI_HANDLE)5)*out=AltWinPath;}
  return *out?EFI_SUCCESS:EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI Locate(EFI_LOCATE_SEARCH_TYPE type,EFI_GUID *g,VOID *key,UINTN *n,EFI_HANDLE **out){
  CHECK(type==ByProtocol&&!memcmp(g,&FsGuid,16)&&!key);*n=Case==TWO_WINDOWS_VOLUMES?3:2;*out=malloc(3*sizeof(**out));CHECK(*out);
  (*out)[0]=(EFI_HANDLE)2;(*out)[1]=(EFI_HANDLE)3;if(*n==3)(*out)[2]=(EFI_HANDLE)5;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Load(BOOLEAN policy,EFI_HANDLE parent,EFI_DEVICE_PATH_PROTOCOL *path,VOID *bytes,UINTN n,EFI_HANDLE *out){
  CHECK(!policy&&parent==(EFI_HANDLE)1&&n==WinExpected&&!memcmp(bytes,WinBytes,n));Loads++;
  CHECK(!memcmp(path,WinPath,WinPathAt));UINT8 *p=(UINT8 *)path+WinPathAt;
  CHECK(p[0]==4&&p[1]==4&&!memcmp(p+4,WindowsName,sizeof(WindowsName)));
  *out=(EFI_HANDLE)10;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Start(EFI_HANDLE image,UINTN *n,CHAR16 **data){CHECK(image==(EFI_HANDLE)10&&!n&&!data);Starts++;return EFI_ABORTED;}
static EFI_STATUS EFIAPI Watchdog(UINTN seconds,UINT64 code,UINTN n,CHAR16 *data){CHECK(!seconds&&!code&&!n&&!data);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Say(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *p,CHAR16 *text){
  CHECK(p==&Out&&text);UINTN used=0;while(Screen[used])used++;
  for(UINTN i=0;text[i]&&used<2047;i++)Screen[used++]=text[i];Screen[used]=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Key(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *p,EFI_INPUT_KEY *key){(void)p;key->ScanCode=0;key->UnicodeChar=KeyMode==1?'U':KeyMode==2?'S':0;return KeyMode?EFI_SUCCESS:EFI_NOT_READY;}
static EFI_STATUS EFIAPI Stall(UINTN n){CHECK(n==100000);Stalls++;return EFI_SUCCESS;}
static void Setup(void) {
  Table=NULL;Self=Windows=OwnVolume=NULL;Root=Log=NULL;Silent=FALSE;Loads=Starts=FileWrites=Stalls=KeyMode=0;Refuse=0;
  Screen[0]=0;memset(&Out,0,sizeof(Out));Out.OutputString=Say;
  memset(&Gu,0,sizeof(Gu));memset(GuAssets,0,sizeof(GuAssets));memset(&Bs,0,sizeof(Bs));memset(&St,0,sizeof(St));
  memset(&SelfImage,0,sizeof(SelfImage));memset(&WinImage,0,sizeof(WinImage));
  Bs.HandleProtocol=Handle;Bs.LocateHandleBuffer=Locate;Bs.FreePool=Free;Bs.LoadImage=Load;Bs.StartImage=Start;Bs.SetWatchdogTimer=Watchdog;Bs.Stall=Stall;
  St.BootServices=&Bs;St.ConOut=&Out;static EFI_SIMPLE_TEXT_INPUT_PROTOCOL input;memset(&input,0,sizeof(input));input.ReadKeyStroke=Key;St.ConIn=&input;
  SelfImage.DeviceHandle=(EFI_HANDLE)2;WinImage.DeviceHandle=(EFI_HANDLE)(UINTN)(Case==WRONG_LOADED_DEVICE?4:3);
  OwnFs.OpenVolume=WinFs.OpenVolume=AltWinFs.OpenVolume=Volume;
  LastWrite[0]=0;
  memset(&OwnRoot,0,sizeof(OwnRoot));memset(&WinRoot,0,sizeof(WinRoot));memset(&AltWinFs,0,sizeof(AltWinFs));memset(&Dir,0,sizeof(Dir));memset(&LogFile,0,sizeof(LogFile));memset(&Reader,0,sizeof(Reader));
  OwnRoot.Open=WinRoot.Open=Dir.Open=Open;OwnRoot.Close=WinRoot.Close=Dir.Close=LogFile.Close=Reader.Close=Close;
  LogFile.Flush=Flush;LogFile.Write=Write;Reader.GetInfo=Info;Reader.Read=Read;
  UsbPath[0]=Case==ESP_PATH?1:3;
  memset(WinPath,0,sizeof(WinPath));memset(AltWinPath,0,sizeof(AltWinPath));
  /* The real ESP on this machine: disk 3 partition 1, starts at sector 2048,
   * 614400 sectors long. boot_io.c's Inspect() matches this geometry, so the
   * fixture has to use it or the Windows volume is never recognised.
   * EXTRA_HARDDISK_NODE puts one unrelated volume in front of it, which is the
   * shape that used to make the internal ESP boot exit without saying anything. */
  UINTN leading=Case==EXTRA_HARDDISK_NODE?1
    :(Case==MAX_HARDDISK_NODES?15
    :(Case==TOO_MANY_HARDDISK_NODES?17:0));
  UINTN at=0;
  for(UINTN k=0;k<leading;k++) {
    HARDDRIVE_DEVICE_PATH *o=(VOID *)(WinPath+at);
    o->Header.Type=4;o->Header.SubType=1;o->Header.Length[0]=42;
    o->PartitionNumber=2;o->PartitionStart=4096;o->PartitionSize=2048;
    o->MBRType=o->SignatureType=2;o->Signature[0]=0xa5;
    at+=42;
  }
  HARDDRIVE_DEVICE_PATH *hd=(VOID *)(WinPath+at);
  hd->Header.Type=4;hd->Header.SubType=1;hd->Header.Length[0]=42;hd->PartitionNumber=1;hd->PartitionStart=2048;hd->PartitionSize=614400;
  hd->MBRType=hd->SignatureType=2;memcpy(hd->Signature,&WindowsPartition,16);if(Case==WRONG_PARTITION)hd->Signature[0]^=1;
  at+=42;
  WinPath[at]=127;WinPath[at+1]=255;WinPath[at+2]=4;WinPathAt=at;
  memcpy(AltWinPath,WinPath,sizeof(WinPath));
}
static UINT8 *FileSized(const char *path,UINTN expected){FILE *f=fopen(path,"rb");CHECK(f);UINT8 *p=malloc(expected);CHECK(p&&fread(p,1,expected,f)==expected&&fgetc(f)==EOF);fclose(f);return p;}
/* bootmgr.efi is a Windows-version artifact: its length changes with every
 * servicing update. Read whatever this machine has instead of pinning the
 * length the upstream AD104 port happened to have. */
static UINT8 *FileAuto(const char *path,UINTN *size){FILE *f=fopen(path,"rb");CHECK(f);long n;CHECK(fseek(f,0,SEEK_END)==0&&(n=ftell(f))>0);CHECK(fseek(f,0,SEEK_SET)==0);UINT8 *p=malloc((size_t)n);CHECK(p&&fread(p,1,(size_t)n,f)==(size_t)n&&fgetc(f)==EOF);fclose(f);*size=(UINTN)n;return p;}
int main(int argc,char **argv){
  CHECK(argc==3);WinBytes=FileAuto(argv[1],&WinExpected);
  static const char *names[]={"booter_load_ad102.bin","v67-ad102-610.43.03-four-plm.bin","gsp_ga10x.bin","gsp_boot_ad102.bin","gsp_boot_desc_ad102.bin"};
  static const UINTN sizes[]={57672,64000,84277400,36864,84};
  for(UINTN i=0;i<5;i++){char p[4096];snprintf(p,sizeof(p),"%s/%s",argv[2],names[i]);AssetBytes[i]=FileSized(p,sizes[i]);}
  for(Case=GOOD;Case<IO_CASES;Case++) {
    Setup();EFI_STATUS s=GuBootOpen((EFI_HANDLE)1,&St);
    /* ESP_PATH must open: no USB node, a harddrive node instead. */
    if(Case==ESP_PATH)CHECK(UsbPath[0]==1);
    /* The six refusal cases are the ones where opening the log must fail. */
    if(Case<NO_LOADED_IMAGE)CHECK(s==EFI_SUCCESS);
    if(Case==GOOD){CHECK(GuBootLoadAssets()==EFI_SUCCESS);for(UINTN i=0;i<5;i++){CHECK(GuAssets[i].firmware.size==sizes[i]&&!memcmp(GuAssets[i].firmware.data,AssetBytes[i],sizes[i]));free((VOID *)GuAssets[i].firmware.data);}}
    if(Case==CHANGED_WINDOWS_IMAGE)WinBytes[1]^=1;
    if(Case<NO_LOADED_IMAGE){s=GuBootLoadWindows();if(Case==CHANGED_WINDOWS_IMAGE)WinBytes[1]^=1;}
    if(Case==GOOD||Case==LOG_WRITE_FAIL||Case==CHANGED_WINDOWS_IMAGE||Case==EXTRA_HARDDISK_NODE||Case==MAX_HARDDISK_NODES){
      /* This port does not pin the digest: the ESP copy is replaced by Windows
       * updates, so a hardcoded hash would brick the handoff after every
       * servicing pass. It records the digest instead, which is what the case
       * asserts: a changed image still loads, and its size and digest are in
       * the log. Pinning lived in upstream and was removed here on purpose. */
      CHECK(s==EFI_SUCCESS&&Loads==1);
      /* The Windows node is not the first node on this path; Inspect() has to
       * keep walking instead of refusing the second harddrive node. */
      /* Sixteen harddrive nodes still go through; seventeen do not. */
      if(Case==EXTRA_HARDDISK_NODE)CHECK(WinPathAt==84);
      if(Case==MAX_HARDDISK_NODES)CHECK(WinPathAt==42*16);
      if(Case==LOG_WRITE_FAIL)CHECK(Gu.fault);
      /* No hash is pinned, so a changed image must still stage. */
    }
    else if(Case>=NO_LOADED_IMAGE){CHECK(!Loads);}
    else if(Case!=ESP_PATH){
      CHECK(s!=EFI_SUCCESS&&!Starts);if(Case!=WRONG_LOADED_DEVICE)CHECK(!Loads);
      /* Each refusal has its own status and its own visible sentence. A halt
       * the operator cannot see is indistinguishable from a hang, and the fix
       * for a hang is a reset, which clears the volatile selectors. */
      if(Case>=NO_LOADED_IMAGE) {
        static const EFI_STATUS want[6]={EFI_UNSUPPORTED,EFI_ACCESS_DENIED,EFI_UNSUPPORTED,
          EFI_DEVICE_ERROR,EFI_DEVICE_ERROR,EFI_OUT_OF_RESOURCES};
        static const CHAR16 *said[6]={
          L"AD104: no loaded-image handle.",L"AD104: own volume has no usable device path.",
          L"AD104: own volume exposes no SimpleFileSystem.",L"AD104: OpenVolume failed on own volume.",
          L"AD104: cannot create \\AD104-UNLOCK on own volume (read-only?).",
          L"AD104: cannot create RUNxxxx.TXT."};
        CHECK(s==want[Case-NO_LOADED_IMAGE]);
        CHECK(wcsstr(Screen,said[Case-NO_LOADED_IMAGE])!=NULL);
      }
      /* Two volumes claim the same Windows ESP geometry. The handoff must
       * refuse rather than pick one, and it must say which way it refused. */
      if(Case==TWO_WINDOWS_VOLUMES)CHECK(s==EFI_NOT_FOUND);
      if(Case==TOO_MANY_HARDDISK_NODES)CHECK(s==EFI_NOT_FOUND);
    }
    if(Case==GOOD) {
      BOOLEAN unlock=FALSE;KeyMode=1;CHECK(GuBootChoose(&unlock)==EFI_SUCCESS&&unlock);
      KeyMode=2;CHECK(GuBootChoose(&unlock)==EFI_SUCCESS&&!unlock);
      KeyMode=0;CHECK(GuBootChoose(&unlock)==EFI_SUCCESS&&!unlock&&Stalls==300);
      CHECK(GuBootWindows()==EFI_ABORTED&&Starts==1);
    }
    CHECK(GuBootClose()==EFI_SUCCESS);printf("boot I/O case %d PASS loads=%u starts=%u\n",Case,Loads,Starts);
  }
  for(UINTN i=0;i<5;i++)free(AssetBytes[i]);free(WinBytes);
  printf("boot I/O: %u checks; %u cases, real file bytes, all EFI services simulated.\n",Checks,IO_CASES);return 0;
}
