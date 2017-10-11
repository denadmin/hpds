/*****************************************************************************
 * Copyright (C) 2014 James Prister
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 *****************************************************************************/

#define DBGINT
// QueueUserAPC(debugAPC, debug_thread, (ULONG_PTR)__LINE__);
//printf("%s %i\n", __FUNCTION__, __LINE__)
#define _WIN32_WINNT 0x0501
#define _WIN32_IE 0x0500
#define GCRYPT_NO_DEPRECATED
#include <d3d9.h>
#include <stdio.h>
#include <stdint.h>
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>
#include <windowsx.h>
#include <commctrl.h>
#include "x264\x264.h"
//#include "portaudio\include\portaudio.h"
#include "ffmpeg-2.0.1\libavcodec\avcodec.h"
#include "ffmpeg-2.0.1\libswscale\swscale.h"
#include "opus-1.0.3\include\opus.h"
#include "libgcrypt-1.5.3\src\gcrypt.h"
#include <time.h>
#include <ole2.h>
#include <shlobj.h>
#include <shlwapi.h>
#include "resource.h"

int fps_goal=32;
int max_bitrate=1000; // (in kilobytes per second. set to 0 to turn off bitrate limiter.)
int crf_goal=25;
int opus_complexity=10;
int opus_bitrate=512000; // in bits/s
int bandwidth_goal=31250; // dynamically adjusted based on values above

#define MTU_SIZE 1500

/*
  ff_is_hwaccel_pix_fmt

  Other ways to limit bit rate:
  Shrink video dimensions.
  Reduce FPS / drop frames.
  --slice-max-size for packet loss resiliency. set it to 1500 to make 1 NALU = 1 packet. Higher values reduce overhead, lower values add packet loss resilience.
*/

#define MsgBox(args...) if (TRUE) { DBGINT; sprintf(volatile_buf, args); MessageBox(mainWindow,volatile_buf,"Alert",MB_OK); }
#define Alert(args...) if (TRUE) { DBGINT; MsgBox(args); }
#define MAlert(args...) if (TRUE) { DBGINT; sprintf(volatile_buf, args); CloseHandle(CreateThread(NULL, 0, ModelessMsgBox, (LPVOID)strdup(volatile_buf), 0, NULL)); }
#define Debug(fmt, args...) if (TRUE) { DBGINT; sprintf(volatile_buf, "function:%s line:%i debug:"fmt, __FUNCTION__, __LINE__, ##args); SetWindowText(mainWindow,volatile_buf); }
#define LIMIT(a,b,c) (a < b ? b : (a > c ? c : a))

#define MEMORY_BARRIER __sync_synchronize
// asm volatile("" : : : "memory")
// The above is a software barrier only; has no effect on runtime CPU reordering.

#define BITSPERPIXEL 32
#define MODE_DIRECTX 1
#define MODE_GDI 2
#define SUBMODE_BLT 1
#define SUBMODE_SETDIBITS 2

int capture_mode=MODE_GDI;
int display_mode=MODE_GDI;
int GDI_submode=SUBMODE_SETDIBITS;
BOOL limit_frame_output=FALSE;
BOOL limit_frame_capture=TRUE;
BOOL scale=TRUE;
int scale_mode=SWS_FAST_BILINEAR;
BOOL encryption=TRUE;
BOOL audio_enabled=TRUE;
BOOL fast_decode=FALSE;

#define TRANSMIT_BUFFER_SIZE 1000000 // the buffer used to encrypt data before it goes to send(). smaller values will cause the application use malloc() more frequently
#define MAX_SOCKET_BUFFER_SIZE 10000000
int tcpThrottling=0;
/*
  TCP window size should be large enough so we don't degrade the performance of the connection, but not so big that the server keeps pumping data to the client when it can't keep up (introducing a response latency) to the user.
  SOCKET_BUFFER_SIZE bytes can be sent before an acknowledge. 20ms delay means that SOCKET_BUFFER_SIZE bytes are sent, then we wait 20ms for a response before sending anymore.
  Thus, we can send a theoretical maximum of SOCKET_BUFFER_SIZE * 1000ms/20ms bytes per second.
  Max bandwidth in kbps = SOCKET_BUFFER_SIZE / (latency in ms)

or rather:

  SOCKET_BUFFER_SIZE = Max connection bandwidth in kbps * (latency in ms)           [the bandwidth-delay product]
  this should be the SO_SNDBUF on the server side - setting "Max connection bandwidth" higher than the realistic bandwidth of the pipe will introduce delay into the system

  SOCKET_BUFFER_SIZE = data processing bandwidth in kbps * (latency in ms)           [the bandwidth-delay product]
  this should be the SO_RCVBUF on the server side - can be calculated by measuring TRANSMIT_AUDIO and TRANSMIT_VIDEO data and dividing it by the processing time of UpdateMainWindow and play_audio_buffer
  exceeding the realistic processing bandwidth will also introduce delay into the system
*/

#define VRECV_BUFFER_SIZE 1000000 // the buffer used to hold video data after recv() and before processing
#define ARECV_BUFFER_SIZE 1000000 // the buffer used to hold audio data after recv() and before processing
#define ASEND_BUFFER_SIZE 1000000 // the buffer used to hold audio data prior to send() and after encoding
#define RECV_BUFFER_SIZE  100000 // MUST BE SMALLER THAN TRANSMIT_BUFFER_SIZE! the buffer used to hold command data after recv() and before processing (includes clipboard data and file data)
#define MAX_CLIPBOARD_SIZE 15000000 // also max filelist size

#define SOCK_DISCONNECTED -1
//#define MAINWINDOWSTYLE WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN | WS_VISIBLE
#define MAINWINDOWSTYLE DS_3DLOOK | DS_CENTER | DS_MODALFRAME | DS_SHELLFONT | WS_CAPTION | WS_VISIBLE | WS_GROUP | WS_SYSMENU
#define MAINWINDOWSTYLE_PLAYER MAINWINDOWSTYLE | WS_MAXIMIZEBOX | WS_THICKFRAME
#define MAINWINDOWSTYLE_PLAYER_FULLSCREEN WS_POPUP | WS_VISIBLE
#define MENU_DISCONNECT 1234
#define MENU_PASSWORD 1235
NOTIFYICONDATA taskIcon = {};
#define TASKTRAY_MESSAGE WM_APP+1
HINSTANCE hInst=NULL;
IDropTarget *dropTarget;

void __cyg_profile_func_enter (void *this_fn, void *call_site) __attribute__((no_instrument_function));
void __cyg_profile_func_exit  (void *this_fn, void *call_site) __attribute__((no_instrument_function));

void get_x264_stream_input (HDC* hDCScreen,HDC* hDCScreenCopyBuffer,HWND* Desktop,HBITMAP* Bitmap, BYTE** desktopBitmapData);
void createMainWindow (HINSTANCE hInstance);
void start_x264_stream_display (void);
void destroyMainWindow(void);

#define TRANSMIT_CMD 0
#define TRANSMIT_VIDEO 1
#define TRANSMIT_AUDIO 2
#define TRANSMIT_CURSOR 3

int transmit(SOCKET s, void *ptr, int len, BYTE transmitType);
int transmit_bulk(SOCKET s, void *ptr, int len, BYTE transmitType);
int transmit_bulk_insitu(SOCKET s, int len, BYTE transmitType);
int transmit_data(SOCKET s, void *ptr, int len, BYTE transmitType, BOOL nodelay, BOOL insitu);

typedef struct transmit_buffer {
  int len;
  BYTE transmitType;
  BYTE data[TRANSMIT_BUFFER_SIZE];
} transmit_buffer;

transmit_buffer *transbuf=NULL;

void initialize_gdi_output(void);
int dWidth=1600, dHeight=900, wWidth=800, wHeight=452;
HDC hDCScreen=NULL, hDCScreenCopyBuffer, mainhDC;
INT_PTR CALLBACK PasswordProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam);
HWND aDesktop, nextClipViewer=NULL, tmpWindow=NULL, mainWindow=NULL, drawWindow=NULL, hwndIP, hwndConnect, hwndListen, hwndText, hwndFPS, hwndFPSL, hwndCRF, hwndCRFL, hwndBITRATE, hwndBITRATEL, hwndAC, hwndACL, hwndABL, hwndABL, hwndABM, hwndABLL1, hwndABLL2, hwndABITRATE, hwndABITRATEL, hwndSCALE, hwndSCALEL, hwndSPEED, hwndFD, hwndSPEEDL, hwndACB, hwndACBL, hwndAudioDevice, hwndAudioDeviceL;

#define TRANSMIT_CF_HTML -1
#define TRANSMIT_CF_RTF -2
UINT CF_IDLIST=0;
UINT CF_HTML=0;
UINT CF_RTF=0;
UINT CF_FILECONTENTS=0;
UINT CF_FILEDESCRIPTOR=0;

BOOL ignoreClip=FALSE;
HFONT mFont;
ATOM atomClassName;
char classname[]="StreamerWindow";
HBITMAP aBitmap,oldBitmap;
HMODULE shell32=NULL;
RECT desktopRect,mwRect;
BYTE* desktopBitmapData;
BYTE *inputBitmapData;
clock_t sizeTimer=0;
int winX, winY;
int terr;

int totalSent=0;
BOOL clientTimeInit;
clock_t ctoLow,ctoHigh,clientLatency,avgClientLatency=1;
void timesync(void);

#define PASSPHRASE_DIGEST_ALGO GCRY_MD_WHIRLPOOL
#define passphrase_length 64
char passphrase[passphrase_length];
BOOL passphrase_set=FALSE;

HDC hdcWindowPrebuffer;
HBITMAP windowBitmap=NULL;
BYTE* windowBitmapData;

IDirect3D9* DXObj;
IDirect3DDevice9* d3dDevice=NULL, *d3dDevice2=NULL, *d3dcapturedevice=NULL;
D3DDISPLAYMODE d3ddm;

x264_picture_t pic_in, pic_out;
uint8_t *oldImgPtr;
x264_t* x264encoder=NULL;

typedef struct datablock {
  BYTE *data;
  int size;
} datablock;

typedef struct datablocks {
  BYTE *data1;
  int size1;
  BYTE *data2;
  int size2;
} datablocks;

#define DATATYPE_CLIPBOARD_BITMAP 1
#define DATATYPE_FILE 2
#define DATATYPE_FILELIST 3

typedef struct datalist {
  int size;
  int offset;
  int index;
  BYTE type;
  void *next;
} datalist;
#define dlist_dataptr(dlist) ((void *)(((BYTE*)dlist)+sizeof(datalist)))
#define dlist_dataptro(dlist,offset) ((void *)(((BYTE*)dlist)+sizeof(datalist)+offset))
#define objOffset(obj,offset) ((void *)(((BYTE*)obj)+sizeof(*obj)+offset))

datalist *transferList=NULL;

AVCodecContext *avCodecContext=NULL;
AVFrame *picture;
AVPicture converted;
struct SwsContext *img_convert_ctx, *cCtx[4]={NULL,NULL,NULL,NULL};

char volatile_buf[5000];
BYTE *recvbuffer;

HANDLE socketEvent;
SOCKET clientsocket=SOCK_DISCONNECTED; // outgoing socket
SOCKET serversocket=SOCK_DISCONNECTED; // listener socket
BOOL serverActive=FALSE;
BOOL clientActive=FALSE;
BOOL captureMouseMove=TRUE;
BOOL cryptActive=TRUE;
BOOL serverResized=FALSE;
BOOL reinitscaling=FALSE;

CRITICAL_SECTION mouselock, nbslock, oframelock, ptclock, paAPClock, abuflock, benchlock, byteslock, audiocapturelock, updatewinlock, paintwinlock, encoderlock, scalecounterlock, scalestartlock, scalestoplock, fmtcounterlock, fmtstartlock, fmtstoplock, cryptlock, cryptshutdownlock, sendlock, transferlock, flistlock;
int scalesComplete=4;
int fmtComplete=4;
BOOL fcsEngaged=FALSE;
BOOL sslEngaged=FALSE;
HANDLE client_thread=NULL;
static HANDLE fmt_threads[4]={NULL,NULL,NULL,NULL};
static HANDLE scale_threads[4] = {NULL,NULL,NULL,NULL};

OpusEncoder *aenc=NULL;
OpusDecoder *adec;

void release_transfer_variables(void);
void release_audio_variables(void);
void release_allocated_variables(void);
void start_client(void);
void start_server(void);
BOOL decode_frame(uint8_t *inbuf,int size, datablock *rframe);
datablock *screen_capture_frame(void);
void setMuxInput(int wavInputDeviceId, int mixerControlId, int numItems, int itemId);

typedef struct cursorData {
  int xHS;
  int yHS;
  int width;
  int height;
  int bpp;
  HCURSOR handle;
} cursorData;
void checkCursor(void);
void setCursor(cursorData *dcursor, int size);
void resetCursor(void);


typedef struct cmdStruct {
  int cmd;
  int param1;
  int param2;
} cmdStruct;

#define CMD_CHANGESIZE 1
#define CMD_RESTARTSTREAM 2
#define CMD_MOUSEMOVE 3
#define CMD_MOUSECLICK 4
#define CMD_KEYDOWN 5
#define CMD_KEYUP 6
#define CMD_REBUFFER 7
#define CMD_TIMESYNC 8
#define CMD_CLIPBOARD 9
#define CMD_SETTCPWINDOW 10
#define CMD_DRAGDROP 11
#define CMD_BANDWIDTH_GOAL 12


#define CMD_MOUSECLICK_LBUTTON 1
#define CMD_MOUSECLICK_RBUTTON 2
#define CMD_MOUSECLICK_MBUTTON 3
#define CMD_MOUSECLICK_WHEEL 4
#define CMD_MOUSECLICK_UP 1
#define CMD_MOUSECLICK_DOWN 2
#define CMD_SETTCPWINDOW_SET 1
#define CMD_SETTCPWINDOW_UNSET 2
#define CMD_DRAGDROP_START 1 // client to server - start
#define CMD_DRAGDROP_GIVEFEEDBACK 2  // server to client - param2 contains new dwEffect
#define CMD_DRAGDROP_STOP 3 // client to server - stop
#define CMD_DRAGDROP_DROP 4 // client to server - drop
#define CMD_DRAGDROP_GET 5 // client to server - drop
#define CMD_DRAGDROP_SEND 6
#define CMD_DRAGDROP_COMPLETE 7 // server to client

BOOL dragging=FALSE;

HHOOK hook;

typedef struct cBuffer {
  int start;
  int cutoff;
  int end;
  int size;
  BOOL blocks;
  BOOL aligned;
  CRITICAL_SECTION lock;
  HANDLE event;
  BYTE *b;
} cBuffer;

cBuffer *abuffer, *vbuffer=NULL;

//int waveHeaderBufferSize=44100*2*2/30; // fixed 30 FPS for audio
// opus must use frame (buffer) sizes of 2.5, 5, 10, 20, 40 or 60 ms. Use a minimum of 10ms, otherwise opus messes up the audio
#define frameDuration 10 // in ms
int waveHeaderBufferSize=48000*2*2 * frameDuration / 1000; // fixed 10ms buffer for audio
int waveBufferFrameSize = 48000      * frameDuration / 1000; // fixed 10ms buffer for audio
#define maxAudioOutputBitrate 1536 // in kilobits per second default: 1536Kbps, which is the uncompressed size of the stream
int maxAudioOutputBytes=maxAudioOutputBitrate*1000/8*frameDuration/1000; // in bytes per frame. used during encoding. default: 48000  * frameDuration / 1000*2*2
int maxWaveBufferFrameSize = 48000   * 120/ 1000; // must be equal to aencdecbuf size
#define AENCDECBUF_SIZE (48000 * 120 / 1000*2*2) // must be the greater of maxWaveBufferFrameSize*4 and maxAudioOutputBytes
BYTE aencdecbuf[AENCDECBUF_SIZE]; 
void get_CELT_stream_input(void);
BOOL audio_recording_active=FALSE;
typedef enum { UNPREPARED, PREPARED } hdrState;
typedef struct hdrInfo {
  WAVEHDR hdr;  
  volatile hdrState state;
} hdrInfo;
#define WAVEINHDRS 10
hdrInfo whdrs[WAVEINHDRS];
HWAVEIN waveIn=NULL;
HWAVEOUT waveOut=NULL;
BOOL audio_playback_paused=TRUE;
BOOL audio_playback_active=FALSE;
void start_CELT_stream_output(void);
int AUDIO_BUFFER=20; // delay time to play in milliseconds, ie. buffer for 20ms before starting playback
int AUDIO_BUFFER_UPPER_LIMIT=80; // if buffer retains this many ms of data, we are really behind. clear it out and drop incoming audio 
float msec_enqueued=0;

int debug_logs[101] = { 0 };
DWORD tlsIndex=0;
int d_counter=-1;
void debug_counter(short int line) {
/*
  int i;
  if (!(i=TlsGetValue(tlsIndex))) {
    i=TlsSetValue(tlsIndex,InterlockedIncrement(&d_counter));
  }
*/
}
HANDLE debug_thread=NULL;
BOOL dbgActive=TRUE;
VOID CALLBACK debugAPC(ULONG_PTR dwParam) {
  if (dbgActive) {
    debug_logs[d_counter++]=(int)dwParam;
    if (d_counter>99) d_counter=0;
  }
}

DWORD WINAPI debugThread(LPVOID in) {
  CRITICAL_SECTION f=flistlock;
  while (TRUE) {
    SleepEx(INFINITE,TRUE);
/*
    if (memcmp(&f,&flistlock,sizeof(CRITICAL_SECTION))) { DBGINT;
      Alert("error");
    }
*/
  }
}

#define clock() clock_w()
LARGE_INTEGER counts_per_sec;
clock_t clock_w() { DBGINT;
  LARGE_INTEGER a;
  DWORD b;
  clock_t c;
  static DWORD offset1=0;
  static clock_t offset2=0;
  static BOOL init=FALSE;

  QueryPerformanceCounter(&a);
  b=GetTickCount()-offset1;
  c=(a.QuadPart*1000.0/counts_per_sec.QuadPart)-offset2;
  if (!init) { DBGINT;
    offset1=b; b=0;
    offset2=c; c=0;
    init=TRUE;
  }
  else if ((((int)c) < ((int)b)-50) || (((int)c) > ((int)b)+50)) { DBGINT; // QueryPerformanceCounter had a lot of drift; needs correction
    offset2+=c-(clock_t)b;
    c=(clock_t)b;
  }

  return c;
}

// FIFO circular buffer functions

/*
int cb_len(cBuffer *buf) { DBGINT;
  return buf->start < buf->end ? buf->end-buf->start : buf->size-buf->start+buf->end;
}
*/

DWORD WINAPI ModelessMsgBox(LPVOID ptr) { DBGINT;
  if (ptr) { DBGINT;
    MessageBox(mainWindow,(char *)ptr,"Alert",MB_OK); 
    free(ptr);
  }
  ExitThread(0);
}

#define cb_totalpending(buf) (buf->start <= buf->end ? buf->end-buf->start : buf->cutoff - buf->start + buf->end) // how much total data is stored in the buffer
#define cb_len(buf) (buf->start <= buf->end ? buf->end-buf->start : buf->cutoff-buf->start) // how much data can be read in one call to cb_read
#define cb_remaining(buf) (buf->start <= buf->end ? buf->size-buf->end : buf->start - buf->end - 1) // how much data can be written in one call to cb_write
#define cb_total_remaining(buf) (buf->start <= buf->end ? buf->size-buf->end+buf->start : buf->start - buf->end - 1) // how much space is available for use in the buffer

cBuffer *cb_create(int size,int aligned) { DBGINT;
  cBuffer *xbuf;

  if (aligned==0) { DBGINT;
    xbuf=malloc(sizeof(cBuffer) + size);
    xbuf->b=(BYTE *)xbuf+sizeof(cBuffer);
  }
  else {
    xbuf=malloc(sizeof(cBuffer));
    xbuf->b=(BYTE *)_aligned_malloc(size,aligned);
  }
  xbuf->start=xbuf->end=0;
  xbuf->cutoff=-1;
  xbuf->size=size;
  xbuf->aligned=aligned ? 1 : 0;
  InitializeCriticalSection(&(xbuf->lock));
  xbuf->event=CreateEvent(NULL,TRUE,FALSE,NULL);
  xbuf->blocks=TRUE;
  return xbuf;
}

void cb_free(cBuffer *xbuf) { DBGINT;
  DeleteCriticalSection(&(xbuf->lock));
  CloseHandle(xbuf->event);
  if (xbuf->aligned) { DBGINT;
    _aligned_free(xbuf->b);
  }
  free(xbuf);
}

#define CB_OVERFLOW -1000
#define CB_INVALIDBLOCKSIZE -1001
datablock cb_recv(SOCKET s, cBuffer *xbuf, int len, int flags) { DBGINT;
  datablock ret;
  int received=0,i;
  BYTE *b=NULL;

  EnterCriticalSection(&(xbuf->lock));
  if (xbuf->start <= xbuf->end) { DBGINT;
    if (xbuf->size-xbuf->end >= len) { DBGINT; // room at the end of the buffer?
      b=xbuf->b+xbuf->end;
      xbuf->end+=len;
    } else if (xbuf->start > len) { DBGINT; // room at the start of the buffer?
      b=xbuf->b;
      xbuf->cutoff=xbuf->end;
      xbuf->end=len;
    }
  } else {
    if (xbuf->start-xbuf->end > len) { DBGINT; // room at the end of the buffer, before the start of the buffer
      b=xbuf->b+xbuf->end;
      xbuf->end+=len;
    }
  }

  if (b==NULL) { DBGINT;
    ret.size=CB_OVERFLOW;
    LeaveCriticalSection(&(xbuf->lock));
    return ret;
  }

  while ((received+=(i=recv(s,(char *)b+received,len-received,flags))) != len) { DBGINT;
    if (i==0 || i==SOCKET_ERROR) { DBGINT;
      ret.size=i;
      LeaveCriticalSection(&(xbuf->lock));
      return ret;
    }
  }

  ret.size=len;
  ret.data=b;
  SetEvent(xbuf->event);
  LeaveCriticalSection(&(xbuf->lock));
  return ret;
}

datablock cb_recv_pad(SOCKET s, cBuffer *xbuf, int len, int flags, int padding, int padbyte) { DBGINT;
  datablock ret;
  int received=0,i;
  BYTE *b=NULL;

  EnterCriticalSection(&(xbuf->lock));
  if (xbuf->start <= xbuf->end) { DBGINT;
    if (xbuf->size-xbuf->end >= len+padding) { DBGINT; // room at the end of the buffer?
      b=xbuf->b+xbuf->end;
      xbuf->end+=len+padding;
    } else if (xbuf->start > len+padding) { DBGINT; // room at the start of the buffer?
      b=xbuf->b;
      xbuf->cutoff=xbuf->end;
      xbuf->end=len+padding;
    }
  } else {
    if (xbuf->start-xbuf->end > len+padding) { DBGINT; // room at the end of the buffer, before the start of the buffer
      b=xbuf->b+xbuf->end;
      xbuf->end+=len+padding;
    }
  }

  if (b==NULL) { DBGINT;
    ret.size=CB_OVERFLOW;
    LeaveCriticalSection(&(xbuf->lock));
    return ret;
  }

  while ((received+=(i=recv(s,(char *)b+received,len-received,flags))) != len) { DBGINT;
    if (i==0 || i==SOCKET_ERROR) { DBGINT;
      ret.size=i;
      LeaveCriticalSection(&(xbuf->lock));
      return ret;
    }
  }

  memset(b+received, padbyte, padding);

  ret.size=len+padding;
  ret.data=b;

  SetEvent(xbuf->event);
  LeaveCriticalSection(&(xbuf->lock));
  return ret;
}
#define cb_write(a,b,c) cb_write_buf(a,b,c,#b)
BYTE *cb_write_buf(BYTE *tb, cBuffer *xbuf, int len, char *name) { DBGINT; // returns NULL if not enough space could be allocated in buffer
  BYTE *b=NULL;

  EnterCriticalSection(&(xbuf->lock));
  if (xbuf->start <= xbuf->end) { DBGINT;
    if (xbuf->size-xbuf->end >= len) { DBGINT; // room at the end of the buffer?
      b=xbuf->b+xbuf->end;
      xbuf->end+=len;
    } else if (xbuf->start > len) { DBGINT; // room at the start of the buffer?
      b=xbuf->b;
      xbuf->cutoff=xbuf->end;
      xbuf->end=len;
    }
  } else {
    if (xbuf->start-xbuf->end > len) { DBGINT; // room at the end of the buffer, before the start of the buffer
      b=xbuf->b+xbuf->end;
      xbuf->end+=len;
    }
  }

  if (b==NULL) { DBGINT;
    LeaveCriticalSection(&(xbuf->lock));
    return NULL;
  }

  memcpy(b,tb,len);
  SetEvent(xbuf->event);
  LeaveCriticalSection(&(xbuf->lock));

  return b;
}

datablocks cb_disjointed_write(BYTE *tb, cBuffer *xbuf, int len, int blocksize) { DBGINT; // only split data up on a blocksize boundary
  int i;
  datablocks ret;
  BYTE *b=NULL;

  ZeroMemory(&ret,sizeof(ret));

  if (blocksize<1) { DBGINT;
    ret.size1=CB_INVALIDBLOCKSIZE;
    return ret;
  }

  EnterCriticalSection(&(xbuf->lock));
  if (cb_total_remaining(xbuf) < len) { DBGINT;
    ret.size1=CB_OVERFLOW;
    LeaveCriticalSection(&(xbuf->lock));
    return ret;
  }

  if (xbuf->end==xbuf->size) xbuf->end=0;

  if (xbuf->start <= xbuf->end) { DBGINT;
    ret.size1=xbuf->size-xbuf->end;
    if (ret.size1>len) ret.size1=len;
    if (ret.size1%blocksize) { DBGINT;
      ret.size1 = ret.size1 - ret.size1%blocksize;
      if (ret.size1 < len && len-ret.size1 > xbuf->start-1) { DBGINT;
        ret.size1=CB_OVERFLOW;
        LeaveCriticalSection(&(xbuf->lock));
        return ret;
      }
    }
    ret.data1=xbuf->b+xbuf->end;
    xbuf->end+=ret.size1;

    if (ret.size1 < len) { DBGINT; // append to start of the buffer
      ret.data2=xbuf->b;
      ret.size2=len-ret.size1;
      xbuf->cutoff=xbuf->end;
      xbuf->end=ret.size2;
    }

    memcpy(ret.data1,tb,ret.size1); // append to end of buffer
    if (ret.size1 < len) // append to start of the buffer
      memcpy(ret.data2,tb+ret.size1,ret.size2); // append to end of buffer      
  } else {
    ret.data1=xbuf->b+xbuf->end;
    ret.size1=len;
    xbuf->end+=ret.size1;

    memcpy(ret.data1,tb,ret.size1); // append to end of buffer
  }

  SetEvent(xbuf->event);
  LeaveCriticalSection(&(xbuf->lock));
  return ret;
}

#define cb_read(a,b,c) cb_read_buf(a,b,c,FALSE,#a).data
#define READ_MAX -12345 // causes a blocking operation
datablock cb_read_buf(cBuffer *xbuf, int i, BOOL advance, BOOL block, char *name) { DBGINT;
  int j;
  datablock ret={0};

  if (i!=READ_MAX && i<0) { DBGINT;
    Alert("Negative cBuffer read!");
    return ret;
  }

  EnterCriticalSection(&(xbuf->lock));
  j=cb_len(xbuf);

  #define BLOCK_OPER do { if (!(xbuf->blocks)) { LeaveCriticalSection(&(xbuf->lock)); return ret; } ResetEvent(xbuf->event); LeaveCriticalSection(&(xbuf->lock)); WaitForSingleObject(xbuf->event,INFINITE); EnterCriticalSection(&(xbuf->lock)); } while (FALSE)

  if (block && i>j) {
    while (i>j) {
      BLOCK_OPER;
      j=cb_len(xbuf);
    }
  }
  else if (i==READ_MAX) {
    if (j==0) {
      BLOCK_OPER;
      j=cb_len(xbuf);
      if (j==0) {
        LeaveCriticalSection(&(xbuf->lock)); 
        return ret; // blocking operation must have been cancelled
      }
    }
    i=j;
  }
  else if (i>j) { DBGINT;
    LeaveCriticalSection(&(xbuf->lock));
    Alert("cBuffer [%s] underflow (len: %i, read: %i) - crashing on purpose to determine etiology", name, cb_len(xbuf), i);
      j=0;
      ret.data=(BYTE *)(1 / j);
    return ret;
  }

  j=xbuf->start;

  if (advance) { DBGINT;
    xbuf->start+=i;

    if (j <= xbuf->end) { DBGINT;
      if (xbuf->start==xbuf->end) { DBGINT;
        xbuf->start=xbuf->end=0;
      }
    } else {
      if (xbuf->start==xbuf->cutoff) { DBGINT;
        xbuf->start=0;
      }
    }
  }
  LeaveCriticalSection(&(xbuf->lock));

  ret.data=xbuf->b+j;
  ret.size=i;
  return ret;
}

/*
#define cb_len(buf) (buf->start <= buf->end ? buf->end-buf->start : buf->size-buf->start+buf->end)
 
void cb_read(BYTE *dest, cBuffer *xbuf, int i) { DBGINT;
  int remaining;
  if (i<0 || cb_len(xbuf)<i) { DBGINT;
    Alert("Illegal circular buffer read operation (read:%i len:%i).", i, cb_len(xbuf));
  }

  if ((remaining=xbuf->size-xbuf->start)>=i) { DBGINT;
    memcpy(dest, xbuf->b+xbuf->start, i);
    xbuf->start+=i;
  } else {
    if (remaining!=0)
      memcpy(dest, xbuf->b+xbuf->start, remaining);
    memcpy(dest+remaining, xbuf->b, i-remaining);
    xbuf->start=i-remaining;
  }
  if (xbuf->start==xbuf->end)
    xbuf->start=xbuf->end=0;
}

void cb_write(BYTE *source, cBuffer *xbuf, int i, BOOL overwrite) { DBGINT;
  int remaining;
  BOOL adjustStart=FALSE;

  if (i<0 || (overwrite && xbuf->size<i) || (!overwrite && xbuf->size<cb_len(xbuf)+i)) { DBGINT;
    Alert("Illegal circular buffer write operation (write:%i len:%i size:%i).", i, cb_len(xbuf), xbuf->size);
    return;
  }

  if (overwrite && xbuf->size<cb_len(xbuf)+i)
    adjustStart=TRUE;

  if ((remaining=xbuf->size-xbuf->end)>=i) { DBGINT;
    memcpy(xbuf->b+xbuf->end, source, i);
    xbuf->end+=i;
  } else {
    if (remaining!=0)
      memcpy(xbuf->b+xbuf->end, source, remaining);
    memcpy(xbuf->b, source+remaining, i-remaining);
    xbuf->end=i-remaining;
  }

  if (adjustStart) { DBGINT;
    if (xbuf->size==xbuf->end+1)
      xbuf->start=0;
    else
      xbuf->start=xbuf->end+1;
  }

  if (xbuf->start==xbuf->end)
    xbuf->start=xbuf->end=0;
}
*/
BOOL fullscreen=FALSE;

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) { DBGINT;
  KBDLLHOOKSTRUCT *k=(KBDLLHOOKSTRUCT *)lParam;
  cmdStruct cmd;
  static RECT olddim;
  HWND desktopWindow;
  HMONITOR mon;
  MONITORINFO minfo;

  if (nCode==0 && GetForegroundWindow()==mainWindow) { DBGINT;
    if (wParam==WM_KEYDOWN || wParam==WM_KEYUP || wParam==WM_SYSKEYDOWN || wParam==WM_SYSKEYUP) { DBGINT;
      if (!serverActive && clientActive) { DBGINT; // we are the client
        if (k->vkCode==VK_RETURN && wParam==WM_SYSKEYDOWN) { DBGINT;
          if (!fullscreen) { DBGINT;
/*
            RECT dimensions;
            GetWindowRect(mainWindow,&olddim);
            desktopWindow = GetDesktopWindow ();
            GetWindowRect (desktopWindow, &dimensions);
            dWidth=dimensions.right-dimensions.left;
            dHeight=dimensions.bottom-dimensions.top;
            SetWindowLong(mainWindow, GWL_STYLE, MAINWINDOWSTYLE_PLAYER_FULLSCREEN);
            MoveWindow(mainWindow,0,0,dWidth,dHeight,TRUE);
*/
            GetWindowRect(mainWindow,&olddim);
            mon=MonitorFromWindow(mainWindow,MONITOR_DEFAULTTONEAREST);
            minfo.cbSize=sizeof(minfo);
            GetMonitorInfo(mon,&minfo);
            dWidth=minfo.rcMonitor.right-minfo.rcMonitor.left;
            dHeight=minfo.rcMonitor.bottom-minfo.rcMonitor.top;
            SetWindowLong(mainWindow, GWL_STYLE, MAINWINDOWSTYLE_PLAYER_FULLSCREEN);
            MoveWindow(mainWindow,minfo.rcMonitor.left,minfo.rcMonitor.top,dWidth,dHeight,TRUE);
          } else {
            SetWindowLong(mainWindow, GWL_STYLE, MAINWINDOWSTYLE_PLAYER);
            MoveWindow(mainWindow,olddim.left,olddim.top,olddim.right-olddim.left,olddim.bottom-olddim.top,TRUE);
          }
          InvalidateRgn(mainWindow, NULL, FALSE);
          UpdateWindow(mainWindow);
          fullscreen=!fullscreen;
          return 1;
        }
        if (wParam==WM_KEYDOWN || wParam==WM_SYSKEYDOWN) cmd.cmd=CMD_KEYDOWN;
        else cmd.cmd=CMD_KEYUP;
        cmd.param1=k->vkCode; // VK code
        cmd.param2=MAKELONG(k->scanCode,k->flags & LLKHF_EXTENDED ? KEYEVENTF_EXTENDEDKEY : 0);
//        cmd.param2=k->flags & LLKHF_EXTENDED ? KEYEVENTF_EXTENDEDKEY : 0;
        transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);
        return 1;
      }
    }
  }
  return CallNextHookEx(0,nCode,wParam,lParam);
}

BOOL gec(gcry_error_t err) { DBGINT;
  char tbuf[100];
  static int i=0;
  i++;
  if (err==GPG_ERR_NO_ERROR)
    return FALSE;
  gpg_strerror_r(err,tbuf,100);
  Alert("Call %i: %s", i, tbuf);
  return TRUE;
}

gcry_sexp_t keypair=NULL;
VOID CALLBACK init_gcrypt(ULONG_PTR dwParam);
HANDLE crypt_thread=NULL;
DWORD windowThreadId;
void WakeThread(HANDLE t);
HANDLE exceptionHandlerHandle;

void SafeShutdown(void) { DBGINT;
  CloseHandle(socketEvent);
  OleUninitialize();
  if (taskIcon.uID==1) { DBGINT;
    Shell_NotifyIcon(NIM_DELETE,&taskIcon);
    DestroyIcon(taskIcon.hIcon);
  }
  if (serversocket!=SOCK_DISCONNECTED) { DBGINT;
    closesocket(serversocket);
    serversocket=SOCK_DISCONNECTED;
  }
  if (clientsocket!=SOCK_DISCONNECTED) { DBGINT;
    closesocket(clientsocket);
    clientsocket=SOCK_DISCONNECTED;
  }
  cryptActive=FALSE;
  if (crypt_thread)
    WakeThread(crypt_thread);
  EnterCriticalSection(&cryptshutdownlock);
  memset(passphrase, 0, passphrase_length);
  WSACleanup();
  UnhookWindowsHookEx(hook);
  RemoveVectoredExceptionHandler(exceptionHandlerHandle);
  ExitProcess(1);
}

#define HEXC(exception) case exception: strcpy(ipcMessage,#exception); sprintf(ipcMessage+strlen(ipcMessage), " %i", dbgX); break;

char ipcMessage[102];
volatile int ipcReady, recoveryAttempt=0;

int dbgX=0;

LONG CALLBACK mainExceptionHandler(PEXCEPTION_POINTERS ExceptionInfo) {
  SECURITY_ATTRIBUTES saAttr; 
  char cmdLine[1000];
  PROCESS_INFORMATION piProcInfo; 
  STARTUPINFO siStartInfo;
  DWORD dwWritten;
  HANDLE stdin_rd,stdin_wr;
  int i;

  if (recoveryAttempt > 0 && (ExceptionInfo->ExceptionRecord->ExceptionCode==EXCEPTION_ACCESS_VIOLATION)) return 0;

  dbgActive=FALSE;
  MEMORY_BARRIER;
  ipcMessage[0]=serverActive ? 1 : 0;

/*
  saAttr.nLength = sizeof(SECURITY_ATTRIBUTES); 
  saAttr.bInheritHandle = TRUE; 
  saAttr.lpSecurityDescriptor = NULL; 

  if (!CreatePipe(&stdin_rd, &stdin_wr, &saAttr, 150)) return 0;
  if (!SetHandleInformation(stdin_wr, HANDLE_FLAG_INHERIT, 0)) return 0; 
*/

  ZeroMemory( &piProcInfo, sizeof(PROCESS_INFORMATION) );
  ZeroMemory( &siStartInfo, sizeof(STARTUPINFO) );
  siStartInfo.cb = sizeof(STARTUPINFO); 

/*
  siStartInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  siStartInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  siStartInfo.hStdInput = stdin_rd;
  siStartInfo.dwFlags |= STARTF_USESTDHANDLES;
*/

  GetModuleFileName(NULL,cmdLine,999);
  cmdLine[999]=0;

  if (serversocket!=SOCK_DISCONNECTED) {
    closesocket(serversocket);
    serversocket=SOCK_DISCONNECTED;
  }
  if (clientsocket!=SOCK_DISCONNECTED) {
    closesocket(clientsocket);
    clientsocket=SOCK_DISCONNECTED;
  }
  WSACleanup();

//  if (CreateProcess(cmdLine,"RECOVERY",NULL,NULL,TRUE,0,NULL,NULL,&siStartInfo,&piProcInfo)) {
  if (CreateProcess(cmdLine,"RECOVERY",NULL,NULL,FALSE,0,NULL,NULL,&siStartInfo,&piProcInfo)) {
    while (ipcReady!=654321) {
      Sleep(1);
      ReadProcessMemory(piProcInfo.hProcess,(void *)&ipcReady,(void *)&ipcReady,sizeof(ipcReady),NULL);
    }
    WriteProcessMemory(piProcInfo.hProcess,(void *)&recoveryAttempt,(void *)&recoveryAttempt,sizeof(recoveryAttempt),NULL);
    WriteProcessMemory(piProcInfo.hProcess,passphrase,passphrase,passphrase_length,NULL);
    memset(passphrase, 0, passphrase_length);
    GetWindowText(hwndIP,volatile_buf,100);

    WriteProcessMemory(piProcInfo.hProcess,ipcMessage,ipcMessage,1,NULL);
    WriteProcessMemory(piProcInfo.hProcess,ipcMessage+1,volatile_buf,strlen(volatile_buf)+1,NULL);
    ipcReady=123456;

    MEMORY_BARRIER; // write-release

    WriteProcessMemory(piProcInfo.hProcess,(void *)&ipcReady,(void *)&ipcReady,sizeof(ipcReady),NULL);
    while (ipcReady!=654321) {
      Sleep(1);
      ReadProcessMemory(piProcInfo.hProcess,(void *)&ipcReady,(void *)&ipcReady,sizeof(ipcReady),NULL);
    }
    switch (ExceptionInfo->ExceptionRecord->ExceptionCode) {
      HEXC(EXCEPTION_ACCESS_VIOLATION);
      HEXC(EXCEPTION_ARRAY_BOUNDS_EXCEEDED);
      HEXC(EXCEPTION_BREAKPOINT);
      HEXC(EXCEPTION_DATATYPE_MISALIGNMENT);
      HEXC(EXCEPTION_FLT_DENORMAL_OPERAND);
      HEXC(EXCEPTION_FLT_DIVIDE_BY_ZERO);
      HEXC(EXCEPTION_FLT_INEXACT_RESULT);
      HEXC(EXCEPTION_FLT_INVALID_OPERATION);
      HEXC(EXCEPTION_FLT_OVERFLOW);
      HEXC(EXCEPTION_FLT_STACK_CHECK);
      HEXC(EXCEPTION_FLT_UNDERFLOW);
      HEXC(EXCEPTION_ILLEGAL_INSTRUCTION);
      HEXC(EXCEPTION_IN_PAGE_ERROR);
      HEXC(EXCEPTION_INT_DIVIDE_BY_ZERO);
      HEXC(EXCEPTION_INT_OVERFLOW);
      HEXC(EXCEPTION_INVALID_DISPOSITION);
      HEXC(EXCEPTION_NONCONTINUABLE_EXCEPTION);
      HEXC(EXCEPTION_PRIV_INSTRUCTION);
      HEXC(EXCEPTION_SINGLE_STEP);
      HEXC(EXCEPTION_STACK_OVERFLOW);
    }
    WriteProcessMemory(piProcInfo.hProcess,ipcMessage,ipcMessage,strlen(ipcMessage)+1,NULL);
    ipcReady=123456;

    MEMORY_BARRIER; // write-release

    WriteProcessMemory(piProcInfo.hProcess,(void *)&ipcReady,(void *)&ipcReady,sizeof(ipcReady),NULL);
    CloseHandle(piProcInfo.hProcess);
    CloseHandle(piProcInfo.hThread);
  }

/*
  i=strlen(passphrase)+1;
  WriteFile(stdin_wr, &i, sizeof(i), &dwWritten, NULL);
  WriteFile(stdin_wr, passphrase, i, &dwWritten, NULL);
  GetWindowText(hwndIP,passphrase,100);
  i=strlen(passphrase)+1;
  WriteFile(stdin_wr, &i, sizeof(i), &dwWritten, NULL);
  WriteFile(stdin_wr, passphrase, i, &dwWritten, NULL);
  switch (ExceptionInfo->ExceptionRecord->ExceptionCode) {
    HEXC(EXCEPTION_ACCESS_VIOLATION);
    HEXC(EXCEPTION_ARRAY_BOUNDS_EXCEEDED);
    HEXC(EXCEPTION_BREAKPOINT);
    HEXC(EXCEPTION_DATATYPE_MISALIGNMENT);
    HEXC(EXCEPTION_FLT_DENORMAL_OPERAND);
    HEXC(EXCEPTION_FLT_DIVIDE_BY_ZERO);
    HEXC(EXCEPTION_FLT_INEXACT_RESULT);
    HEXC(EXCEPTION_FLT_INVALID_OPERATION);
    HEXC(EXCEPTION_FLT_OVERFLOW);
    HEXC(EXCEPTION_FLT_STACK_CHECK);
    HEXC(EXCEPTION_FLT_UNDERFLOW);
    HEXC(EXCEPTION_ILLEGAL_INSTRUCTION);
    HEXC(EXCEPTION_IN_PAGE_ERROR);
    HEXC(EXCEPTION_INT_DIVIDE_BY_ZERO);
    HEXC(EXCEPTION_INT_OVERFLOW);
    HEXC(EXCEPTION_INVALID_DISPOSITION);
    HEXC(EXCEPTION_NONCONTINUABLE_EXCEPTION);
    HEXC(EXCEPTION_PRIV_INSTRUCTION);
    HEXC(EXCEPTION_SINGLE_STEP);
    HEXC(EXCEPTION_STACK_OVERFLOW);
  }
  i=strlen(passphrase)+1;
  WriteFile(stdin_wr, &i, sizeof(i), &dwWritten, NULL);
  WriteFile(stdin_wr, passphrase, i, &dwWritten, NULL);
  CloseHandle(stdin_wr);
*/
  volatile_buf[0]=0;
  for (i=d_counter; i<100; i++)
    sprintf(volatile_buf+strlen(volatile_buf), "%i ", debug_logs[i]);
  for (i=0; i<d_counter; i++)
    sprintf(volatile_buf+strlen(volatile_buf), "%i ", debug_logs[i]);
//  MessageBox(NULL,volatile_buf,"Alert",MB_OK);

  return 0;
}

void gcrypt_loop(void);
#define CRYPT_SLEEP 0
#define CRYPT_AUTH 1
int crypt_mode=CRYPT_SLEEP;

#define CRYPT_PENDING 0
#define CRYPT_SUCCESS 1
#define CRYPT_FAIL 2
int crypt_state=-1;

DWORD WINAPI message_loop(LPVOID hInstance) { DBGINT;
  MSG msg;
  cmdStruct cmd;

  createMainWindow (hInstance);
  windowThreadId=GetCurrentThreadId();
  hook=SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInstance, 0);

  while (TRUE) { DBGINT;
    GetMessage (&msg, NULL, 0, 0);
          if (msg.message==WM_QUIT) { DBGINT;
//            release_allocated_variables();
//            destroyMainWindow();
            SafeShutdown();
          }
          if (tmpWindow && msg.hwnd==tmpWindow) {
            TranslateMessage (&msg);
            DispatchMessage (&msg);
          }
          else if ((!serverActive && clientActive) || !IsDialogMessage(mainWindow, &msg)) { DBGINT;
            if (serverActive || !clientActive) // we are not the client
              TranslateMessage (&msg);
            DispatchMessage (&msg);
            if (sizeTimer>0 && clock()-sizeTimer>250) { DBGINT;
              sizeTimer=0;
              cmd.cmd=CMD_CHANGESIZE;
              cmd.param1=winX;
              cmd.param2=winY;
              transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);
            }
          }
        }
}

int WINAPI WinMain(  HINSTANCE  hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
  WSADATA wsaData;

  tlsIndex=TlsAlloc();

  LoadLibrary("exchndl.dll");
  hInst=hInstance;

  socketEvent=CreateEvent(NULL,FALSE,FALSE,NULL);

  if (!QueryPerformanceFrequency(&counts_per_sec)) { DBGINT;
    Alert("Hardware doesn't support high performance counters.");
    return;
  }

  exceptionHandlerHandle=AddVectoredExceptionHandler(1,mainExceptionHandler);

/* int main(void) { DBGINT;
  WSADATA wsaData;

  hInst=GetModuleHandle(NULL);
*/
  InitializeCriticalSection(&mouselock);
  InitializeCriticalSection(&nbslock);
  InitializeCriticalSection(&oframelock);
  InitializeCriticalSection(&ptclock);
  InitializeCriticalSection(&paAPClock);
  InitializeCriticalSection(&abuflock);
  InitializeCriticalSection(&benchlock);
  InitializeCriticalSection(&byteslock);
  InitializeCriticalSection(&sendlock);
  InitializeCriticalSection(&flistlock);
  InitializeCriticalSection(&transferlock);
  InitializeCriticalSection(&audiocapturelock);
  InitializeCriticalSection(&encoderlock);
  InitializeCriticalSection(&updatewinlock);
  InitializeCriticalSection(&paintwinlock);
  InitializeCriticalSectionAndSpinCount(&scalecounterlock,4000);
  InitializeCriticalSectionAndSpinCount(&scalestartlock,4000);
  InitializeCriticalSectionAndSpinCount(&scalestoplock,4000);
  InitializeCriticalSectionAndSpinCount(&fmtcounterlock,4000);
  InitializeCriticalSectionAndSpinCount(&fmtstartlock,4000);
  InitializeCriticalSectionAndSpinCount(&fmtstoplock,4000);
  InitializeCriticalSection(&cryptlock);
  InitializeCriticalSection(&cryptshutdownlock);

  recvbuffer=(BYTE *)malloc(RECV_BUFFER_SIZE);

  WSAStartup( MAKEWORD(2,2), &wsaData );

/*
  if(Pa_Initialize()!=paNoError) { DBGINT;
    Alert("Error initializing audio subsystem.");
    ExitProcess(0);
  }
*/

  debug_thread=CreateThread(NULL, 0, debugThread, (LPVOID) NULL, 0, NULL);

  CloseHandle(CreateThread(NULL, 0, message_loop, (LPVOID)hInst, 0, NULL));
  gcrypt_loop(); // annoying non-threadsafe module needs message loop in main thread.
}

int xint,aint=1;
void errcheck(HRESULT ret) { DBGINT;
  if (ret != D3D_OK) { DBGINT;
    if (ret == D3DERR_INVALIDCALL) { DBGINT;
      MsgBox("Error on line %i (%i) [INVALIDCALL].",xint,ret);
    }
    else {
      MsgBox("Error on line %i (%i).",xint,ret);
    }
    SafeShutdown();
  }
/*
    MsgBox("Step %i (%i).",xint,ret);
*/
  xint++;
  aint++;
}

#define BENCHMARK_MAX 50
clock_t totalBenchTime,lastbenchmark,lastbenchmarkid=0;
clock_t benchmarks[BENCHMARK_MAX];
char *benchmark_names[BENCHMARK_MAX];
int bCounter=0;
int bCycles=0;
BOOL bStarted=FALSE;

#define BM_START 1
#define BM_STOP 2
void benchmark_range(const char *name, int mode, clock_t *diff) { DBGINT;
  int j;
  clock_t i=clock();

  EnterCriticalSection(&benchlock);
  for (j=0; j<BENCHMARK_MAX; j++)
    if (benchmark_names[j]==name || benchmark_names[j]==NULL)
      break;
  if (j<BENCHMARK_MAX) { DBGINT;
    if (benchmark_names[j]==NULL) { DBGINT; bCounter++; benchmark_names[j]=(char *)name; }
    if (mode==BM_START)
      *diff=i;
    else if (mode==BM_STOP) { DBGINT;
      benchmarks[j]+=i-*diff;
      totalBenchTime+=i-*diff;
      if (totalBenchTime > 10000) { DBGINT;
        bCycles=bCycles * 5000 / totalBenchTime;
        for (j=0; j<bCounter; j++)
          benchmarks[j]=benchmarks[j] * 5000 / totalBenchTime;
        totalBenchTime=5000;
      }
    }
  }
  LeaveCriticalSection(&benchlock);
}

void benchmark_set(const char *name) { DBGINT;
  int j;
  clock_t i=clock();
  EnterCriticalSection(&benchlock);
  for (j=0; j<BENCHMARK_MAX; j++)
    if (benchmark_names[j]==name || benchmark_names[j]==NULL)
      break;
  if (j<BENCHMARK_MAX) { DBGINT;
    if (benchmark_names[j]==NULL) { DBGINT; bCounter++; benchmark_names[j]=(char *)name; }
    benchmarks[lastbenchmarkid]+=i-lastbenchmark;
    totalBenchTime+=i-lastbenchmark;
    lastbenchmark=i;
    lastbenchmarkid=j;
  }
  LeaveCriticalSection(&benchlock);
}

void benchmark_reset(const char *name) { DBGINT;
  int i;
  benchmark_set(name);

  EnterCriticalSection(&benchlock);
  if (totalBenchTime > 10000) { DBGINT;
    bCycles=bCycles * 5000 / totalBenchTime;
    for (i=0; i<bCounter; i++)
      benchmarks[i]=benchmarks[i] * 5000 / totalBenchTime;
    totalBenchTime=5000;
  } else {
    bCycles++;
  }
  LeaveCriticalSection(&benchlock);
}

void benchmark_start(const char *name) { DBGINT;
  if (bStarted) { DBGINT;
    benchmark_reset(name);
    return;
  }
  else {
    FillMemory(benchmark_names,sizeof(char *)*BENCHMARK_MAX,0);
    bStarted=TRUE;
  }
  benchmark_names[0]=(char *)name;
  bCounter=1;
  bCycles=0;
  totalBenchTime=0;
  FillMemory(benchmarks,sizeof(clock_t)*BENCHMARK_MAX,0);
  lastbenchmark=clock();
}

void benchmark_string(char *tbuf) { DBGINT;
  int i;
  if (bCycles>0)
    sprintf(tbuf+strlen(tbuf), "Avg Cycle %ims ", totalBenchTime/bCycles);
  for (i=0;i<bCounter;i++) { DBGINT;
    if (i>0)
      sprintf(tbuf+strlen(tbuf), " ");
    sprintf(tbuf+strlen(tbuf), "[%02.0f%% ", 100.0*benchmarks[i]/totalBenchTime);
    sprintf(tbuf+strlen(tbuf),"%s]", benchmark_names[i]);
  }
}

int oframe=0;
VOID CALLBACK UpdateMainWindow2(ULONG_PTR blen);
VOID CALLBACK UpdateMainWindow(ULONG_PTR blen) { DBGINT;
  static clock_t a=0;
  const char *alabel="Inter-Update";
  if (a!=0)
    benchmark_range(alabel,BM_STOP,&a);
  UpdateMainWindow2(blen);
  EnterCriticalSection(&oframelock);
  oframe--;
  LeaveCriticalSection(&oframelock);
  benchmark_range(alabel,BM_START,&a);
}

int totalBytes=0,totalTime=0;

typedef struct fileInfo {
  char *name;
  union {
    HANDLE file;
    IStream *stream;
  };
  HANDLE event1;
  HANDLE event2;
  WIN32_FILE_ATTRIBUTE_DATA attr;
} fileInfo;

void transferLowPriorityData(int i) {
  int j;
  DWORD read;
  datalist *dlist;
    if (i>0) {
      EnterCriticalSection(&transferlock);
      while (transferList) {
          switch (transferList->type) {
            case DATATYPE_CLIPBOARD_BITMAP:
              while (transferList->offset<transferList->size) { DBGINT;
                j=LIMIT(transferList->size-transferList->offset,0,i-sizeof(cmdStruct));
                if (j<1) {
                  i=0; break;
                }
                EnterCriticalSection(&sendlock);
                  ((cmdStruct *)(transbuf->data))->cmd=CMD_CLIPBOARD;
                  ((cmdStruct *)(transbuf->data))->param1=CF_BITMAP;
                  ((cmdStruct *)(transbuf->data))->param2=(transferList->offset==0 ? transferList->size : 0);
                  memcpy(((BYTE *)(transbuf->data))+sizeof(cmdStruct),((BYTE*)transferList)+sizeof(datalist)+transferList->offset,j);
                  transmit_bulk_insitu(clientsocket,j+sizeof(cmdStruct),TRANSMIT_CMD);
                LeaveCriticalSection(&sendlock);
                transferList->offset+=j;
                if (!(i-=j))
                  break;
              }
              break;
            case DATATYPE_FILELIST:
              while (transferList->offset<transferList->size) { DBGINT;
                j=LIMIT(transferList->size-transferList->offset,0,i-sizeof(cmdStruct));
                if (j<1) {
                  i=0; break;
                }
                EnterCriticalSection(&sendlock);
                  ((cmdStruct *)(transbuf->data))->cmd=CMD_DRAGDROP;
                  ((cmdStruct *)(transbuf->data))->param1=CMD_DRAGDROP_START;
                  ((cmdStruct *)(transbuf->data))->param2=(transferList->offset==0 ? transferList->size : 0);

                  memcpy(((BYTE *)(transbuf->data))+sizeof(cmdStruct),((BYTE*)transferList)+sizeof(datalist)+transferList->offset,j);
                  transmit_bulk_insitu(clientsocket,j+sizeof(cmdStruct),TRANSMIT_CMD);
                LeaveCriticalSection(&sendlock);
                transferList->offset+=j;
                if (!(i-=j))
                  break;
              }
              break;
            case DATATYPE_FILE:
              while (transferList->offset<transferList->size) { DBGINT;
                j=LIMIT(transferList->size-transferList->offset,0,i-sizeof(cmdStruct));
                if (j<1) {
                  i=0; break;
                }
                EnterCriticalSection(&sendlock);
                  ((cmdStruct *)(transbuf->data))->cmd=CMD_DRAGDROP;
                  ((cmdStruct *)(transbuf->data))->param1=CMD_DRAGDROP_SEND;
                  ((cmdStruct *)(transbuf->data))->param2=transferList->index;
                  if (!ReadFile(((fileInfo*)dlist_dataptr(transferList))->file,((BYTE *)(transbuf->data))+sizeof(cmdStruct),j,&read,NULL)) {
                    i=GetLastError();
                    Alert("readfile failed %i",i);
                    SafeShutdown();
                  }
                  j=read;
                  transmit_bulk_insitu(clientsocket,j+sizeof(cmdStruct),TRANSMIT_CMD);
                LeaveCriticalSection(&sendlock);
                transferList->offset+=j;
                if (!(i-=j))
                  break;
              }
              break;
          }
        if (i) {
          dlist=transferList;
          transferList=transferList->next;
          free(dlist);
        } else
          break;
      }
      LeaveCriticalSection(&transferlock);
    }
}

VOID CALLBACK UpdateMainWindow2(ULONG_PTR blen) { DBGINT;
  BYTE* tData;
  static IDirect3DSurface9* pRenderTarget=NULL;
  IDirect3DSurface9* pDestTarget=NULL, *s1=NULL;
  IDirect3DSurface9 *s2, *s3;
  D3DLOCKED_RECT pLockedRect;
  int i,j,k;
  static int totalFPS=0,fullTime=0;
  int cStart;
  BITMAPINFO   bi;
  datablock *capturedFrame;
  x264_nal_t *nals;
  datablock decodedFrame;
  char tbuf[500];
  const char *alabel="Paint";
  const char *blabel="Sleep";
  clock_t a;

  EnterCriticalSection(&updatewinlock);
  if (fullTime==0)
    fullTime=clock();
  cStart=clock();

  xint=1;
  EnterCriticalSection(&ptclock);
  totalBytes+=blen;
  LeaveCriticalSection(&ptclock);
    if (!decode_frame(cb_read(vbuffer,(int)blen+FF_INPUT_BUFFER_PADDING_SIZE,FALSE),(int)blen,&decodedFrame)) { DBGINT;
      cb_read(vbuffer,(int)blen+FF_INPUT_BUFFER_PADDING_SIZE,TRUE);
      LeaveCriticalSection(&updatewinlock);
      return;
    }

    cb_read(vbuffer,(int)blen+FF_INPUT_BUFFER_PADDING_SIZE,TRUE);

    inputBitmapData=decodedFrame.data;

    benchmark_range(alabel,BM_START,&a);

    EnterCriticalSection(&paintwinlock);

    //BitBlt GDI is fastest screen output.
    //DirectX stretch second fastest.
    //StretchBlt slowest method.

    GdiFlush(); // Make sure that BitBlt is done writing the bitmap data before we start reading.

    if (display_mode==MODE_DIRECTX) { DBGINT;
      errcheck(IDirect3DDevice9_BeginScene(d3dDevice));
      errcheck(IDirect3DDevice9_CreateOffscreenPlainSurface(d3dDevice, dWidth, dHeight, d3ddm.Format, D3DPOOL_DEFAULT, &s1, NULL));
      errcheck(IDirect3DSurface9_LockRect(s1, &pLockedRect, NULL, D3DLOCK_DISCARD));
      tData = (BYTE *)pLockedRect.pBits;
      for(i=0 ; i < dHeight ; i++) { DBGINT;
        memcpy( (BYTE*) pLockedRect.pBits + (dHeight-i-1) * pLockedRect.Pitch, inputBitmapData + i * dWidth * BITSPERPIXEL / 8, dWidth * BITSPERPIXEL / 8);
      }
      errcheck(IDirect3DSurface9_UnlockRect(s1));
      errcheck(IDirect3DDevice9_GetBackBuffer(d3dDevice, 0, 0, D3DBACKBUFFER_TYPE_MONO, &pRenderTarget));
      errcheck(IDirect3DDevice9_StretchRect(d3dDevice, s1, NULL, pRenderTarget, NULL, D3DTEXF_NONE));
      errcheck(IDirect3DSurface9_Release(s1));
      errcheck(IDirect3DSurface9_Release(pRenderTarget));
      errcheck(IDirect3DDevice9_EndScene(d3dDevice));
      errcheck(IDirect3DDevice9_Present(d3dDevice, NULL, NULL, NULL, NULL));
    }

    if (display_mode==MODE_GDI) { DBGINT;

      if (GDI_submode==SUBMODE_BLT) { DBGINT;
        for(i=0 ; i < dHeight ; i++) { DBGINT;
          memcpy( windowBitmapData + i * dWidth * BITSPERPIXEL / 8, inputBitmapData + i * dWidth * BITSPERPIXEL / 8, dWidth * BITSPERPIXEL / 8);
        }

        // StretchBlt or BitBlt
        //SetStretchBltMode (mainhDC, HALFTONE);
        //SetBrushOrgEx (mainhDC, 0, 0, NULL);
        //StretchBlt (mainhDC, 0, 0, wWidth, wHeight, hdcWindowPrebuffer, desktopRect.left, desktopRect.top, dWidth, dHeight, SRCCOPY);

//        BitBlt (mainhDC, 0, 0, dWidth, dHeight, hdcWindowPrebuffer, desktopRect.left, desktopRect.top, SRCCOPY);
        BitBlt (mainhDC, 0, 0, dWidth, dHeight, hdcWindowPrebuffer, 0, 0, SRCCOPY);

      }
      if (GDI_submode==SUBMODE_SETDIBITS) { DBGINT;
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);    
        bi.bmiHeader.biWidth = wWidth;    
        bi.bmiHeader.biHeight = wHeight;  
        bi.bmiHeader.biPlanes = 1;    
        bi.bmiHeader.biBitCount = 32;    
        bi.bmiHeader.biCompression = BI_RGB;    
        bi.bmiHeader.biSizeImage = 0;  
        bi.bmiHeader.biXPelsPerMeter = 0;    
        bi.bmiHeader.biYPelsPerMeter = 0;    
        bi.bmiHeader.biClrUsed = 0;    
        bi.bmiHeader.biClrImportant = 0;
        SetDIBitsToDevice(mainhDC,0,0,wWidth,wHeight,0,0,0,wHeight,inputBitmapData,&bi,DIB_RGB_COLORS);
      }
    }

    ValidateRect(mainWindow,NULL);

    LeaveCriticalSection(&paintwinlock);
    LeaveCriticalSection(&updatewinlock);

    benchmark_range(alabel,BM_STOP,&a);

    i=clock();
    EnterCriticalSection(&ptclock);
    totalTime+=i-cStart;
    LeaveCriticalSection(&ptclock);
    totalFPS++;
    if (i-fullTime>1000) { DBGINT;
      EnterCriticalSection(&ptclock);
      j=totalBytes*LIMIT(avgClientLatency,1,1000)/(totalTime==0 ? 1 : totalTime);
      LeaveCriticalSection(&ptclock);
      if (tcpThrottling)
        j=LIMIT(j,1,tcpThrottling);
      else
        j=MAX_SOCKET_BUFFER_SIZE;
      if (clientsocket!=SOCK_DISCONNECTED) {
        setsockopt(clientsocket, SOL_SOCKET, SO_RCVBUF, (char *)&j, sizeof(j)); // how much data to buffer before we start blocking because the client isn't a fast enough decoder
        k=sizeof(j);
        getsockopt(clientsocket, SOL_SOCKET, SO_RCVBUF, (char *)&j, &k);
      }
      sprintf(tbuf,"Streamer %0.2f fps [%0.2f potential] [%.2fkb/sec] [A. Buffer %.0fms] [Rcv Window %i/%i]",totalFPS * 1000.0 / (i-fullTime),totalFPS * 1000.0 / totalTime,1.0*totalBytes/(i-fullTime), msec_enqueued, totalBytes*(avgClientLatency==0 ? 1 : avgClientLatency)/(totalTime==0 ? 1 : totalTime), j);

      if (display_mode==MODE_DIRECTX)
        sprintf(tbuf+strlen(tbuf)," [DIRECTX Display] ");

      if (display_mode==MODE_GDI)
        sprintf(tbuf+strlen(tbuf)," [GDI Display] ");

      benchmark_string(tbuf+strlen(tbuf));
      SetWindowText(mainWindow,tbuf);
      totalTime=totalFPS=totalBytes=0;
      fullTime=i;

      timesync();
    }
    if (totalFPS * 1000.0 / (i-fullTime) > fps_goal && limit_frame_output) {
      benchmark_range(blabel,BM_START,&a);
      Sleep(totalFPS*1000.0/fps_goal+fullTime-i);
      benchmark_range(blabel,BM_STOP,&a);
    }
/*
  if (imageData==NULL)
    imageData=malloc(dWidth*dHeight*BITSPERPIXEL/8);

  errcheck(IDirect3DDevice9_CreateOffscreenPlainSurface(d3dDevice2, dWidth, dHeight, d3ddm.Format, D3DPOOL_SYSTEMMEM, &s1, NULL)); // system
  errcheck(IDirect3DDevice9_GetRenderTarget(d3dDevice2, 0, &pRenderTarget));
  errcheck(IDirect3DDevice9_GetRenderTargetData(d3dDevice2, pRenderTarget, s1)); 

  errcheck(IDirect3DSurface9_LockRect(s1, &pLockedRect, NULL, D3DLOCK_NO_DIRTY_UPDATE | D3DLOCK_NOSYSLOCK | D3DLOCK_READONLY));
  imageData=malloc(dWidth*dHeight*BITSPERPIXEL/8);
  for(i=0 ; i < dHeight ; i++) { DBGINT;
    memcpy( imageData + i * dWidth * BITSPERPIXEL / 8 , (BYTE*) pLockedRect.pBits + i* pLockedRect.Pitch, dWidth * BITSPERPIXEL / 8);
  }
  errcheck(IDirect3DSurface9_UnlockRect(s1));
  errcheck(IDirect3DSurface9_Release(s1));
  errcheck(IDirect3DSurface9_Release(pRenderTarget));
*/

// WORKINGFFINAL
//  errcheck(IDirect3DDevice9_CreateOffscreenPlainSurface(d3dDevice2, dWidth, dHeight, d3ddm.Format, D3DPOOL_SYSTEMMEM, &s1, NULL)); // system
//  errcheck(IDirect3DDevice9_GetRenderTarget(d3dDevice2, 0, &pRenderTarget));
//  errcheck(IDirect3DDevice9_GetRenderTargetData(d3dDevice2, pRenderTarget, s1)); 
//  errcheck(IDirect3DSurface9_Release(pRenderTarget));

//  errcheck(IDirect3DSurface9_LockRect(s1, &pLockedRect, NULL, D3DLOCK_NO_DIRTY_UPDATE | D3DLOCK_NOSYSLOCK | D3DLOCK_READONLY));
//  cStart=clock();

//  for(i=0 ; i < dHeight ; i++) { DBGINT;
//    memcpy( imageData + i * dWidth * BITSPERPIXEL / 8 , (BYTE*) pLockedRect.pBits + i* pLockedRect.Pitch, dWidth * BITSPERPIXEL / 8);
//  }

//  cEnd=clock();
//  errcheck(IDirect3DSurface9_UnlockRect(s1));
//  errcheck(IDirect3DSurface9_Release(s1));
/*
  BITMAPINFOHEADER   bi;

  bi.biSize = sizeof(BITMAPINFOHEADER);    
  bi.biWidth = dWidth;    
  bi.biHeight = dHeight;  
  bi.biPlanes = 1;    
  bi.biBitCount = 32;    
  bi.biCompression = BI_RGB;    
  bi.biSizeImage = 0;  
  bi.biXPelsPerMeter = 0;    
  bi.biYPelsPerMeter = 0;    
  bi.biClrUsed = 0;    
  bi.biClrImportant = 0;
  GetDIBits(hDCScreenCopyBuffer, aBitmap, 0, dHeight, imageData, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
*/

//  errcheck(IDirect3DDevice9_Clear(d3dDevice, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255,0,0), 1, 0));

/*
  for(i=0 ; i < dHeight ; i++) { DBGINT;
    memcpy( (BYTE*) pLockedRect.pBits + i* pLockedRect.Pitch, inputBitmapData + i * dWidth * BITSPERPIXEL / 8, dWidth * BITSPERPIXEL / 8);
  }
  Need to transpose when using DIBs.
*/

/*
  tData = (BYTE *)pLockedRect.pBits;
  for(i=0 ; i < dHeight ; i++)

  for(j=0 ; j < dWidth ; j++) { DBGINT;
    tData[j*4 + i*pLockedRect.Pitch]=0;
    tData[j*4 + i*pLockedRect.Pitch+1]=128;
    tData[j*4 + i*pLockedRect.Pitch+2]=128;
    tData[j*4 + i*pLockedRect.Pitch+3]=128;
  }
*/

//  errcheck(IDirect3DDevice9_UpdateSurface(d3dDevice, s1, NULL, pRenderTarget, NULL));

/*
  errcheck(IDirect3DDevice9_CreateOffscreenPlainSurface(d3dDevice, dWidth, dHeight, d3ddm.Format, D3DPOOL_SYSTEMMEM, &s1, NULL)); // system
  errcheck(IDirect3DDevice9_CreateOffscreenPlainSurface(d3dDevice, dWidth, dHeight, d3ddm.Format, D3DPOOL_DEFAULT, &s2, NULL)); // video

  errcheck(IDirect3DDevice9_GetRenderTarget(d3dDevice2, 0, &pRenderTarget));
  errcheck(IDirect3DDevice9_GetRenderTargetData(d3dDevice2, pRenderTarget, s1));

  errcheck(IDirect3DDevice9_Clear(d3dDevice, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255,0,0), 1, 0));
  errcheck(IDirect3DDevice9_BeginScene(d3dDevice));

  errcheck(IDirect3DDevice9_UpdateSurface(d3dDevice, s1, NULL, s2, NULL));
  errcheck(IDirect3DDevice9_GetBackBuffer(d3dDevice, 0, 0, D3DBACKBUFFER_TYPE_MONO, &pDestTarget));
  errcheck(IDirect3DDevice9_StretchRect(d3dDevice, s2, NULL, pDestTarget, NULL, D3DTEXF_NONE));

  IDirect3DSurface9_Release(pRenderTarget);
  IDirect3DSurface9_Release(pDestTarget);

  errcheck(IDirect3DDevice9_EndScene(d3dDevice));
  errcheck(IDirect3DDevice9_Present(d3dDevice, NULL, NULL, NULL, NULL));

  IDirect3DSurface9_Release(s1);
  IDirect3DSurface9_Release(s2);
*/
/*
  WORKS
  errcheck(IDirect3DDevice9_Clear(d3dDevice, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255,0,0), 1, 0));
  errcheck(IDirect3DDevice9_BeginScene(d3dDevice));

  errcheck(IDirect3DDevice9_CreateOffscreenPlainSurface(d3dDevice, dWidth, dHeight, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &s1, NULL)); // system
  errcheck(IDirect3DDevice9_CreateOffscreenPlainSurface(d3dDevice, dWidth, dHeight, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s2, NULL)); // video
  errcheck(IDirect3DDevice9_GetFrontBufferData(d3dDevice, 0, s1)); 

  errcheck(IDirect3DDevice9_UpdateSurface(d3dDevice, s1, NULL, s2, NULL));
  errcheck(IDirect3DDevice9_GetBackBuffer(d3dDevice, 0, 0, D3DBACKBUFFER_TYPE_MONO, &pDestTarget));
  errcheck(IDirect3DDevice9_StretchRect(d3dDevice, s2, NULL, pDestTarget, NULL, D3DTEXF_NONE));

  errcheck(IDirect3DSurface9_Release(s1));
  errcheck(IDirect3DSurface9_Release(s2));
  errcheck(IDirect3DSurface9_Release(pDestTarget));

  errcheck(IDirect3DDevice9_EndScene(d3dDevice));
  errcheck(IDirect3DDevice9_Present(d3dDevice, NULL, NULL, NULL, NULL));
*/

/*
  hDC=GetDC(mainWindow);
  StretchBlt (hDCScreenCopyBuffer, 0, 0, dWidth/4, dHeight/4, hDCScreen, desktopRect.left, desktopRect.top, dWidth, dHeight, SRCCOPY);
  BitBlt (hDC, 0, 0, dWidth, dHeight, hDCScreenCopyBuffer, desktopRect.left, desktopRect.top, SRCCOPY);
  ReleaseDC(mainWindow,hDC);
*/
}

/*
PaStream *audioStream;
BYTE frameBuffer[2000000];
int frameBufferSize=0,fbcount=0;

static int audioOutputCallback( const void *inputBuffer, void *outputBuffer, unsigned long framesPerBuffer, const PaStreamCallbackTimeInfo* timeInfo, PaStreamCallbackFlags statusFlags, void *userData ) { DBGINT;
    unsigned long i;

    (void) timeInfo;
    (void) statusFlags;
    (void) inputBuffer;

    i=framesPerBuffer*2*4;
    if (fbcount+i > frameBufferSize)
      i=frameBufferSize-fbcount;
    memcpy(outputBuffer,frameBuffer+fbcount,i);
    fbcount+=i;
    if (fbcount==frameBufferSize) { DBGINT;
Alert("done");
      return paComplete;
    }
    else
      return paContinue;
}

void get_audio_input(void) { DBGINT;
  char tbuf[100];
  PaStreamParameters params;
  PaDeviceInfo *devinfo;
  PaStreamInfo *streaminfo;
  signed long inputFrames;
  int i;

  params.device=SendMessage(hwndAudioDevice,(UINT) CB_GETITEMDATA,SendMessage(hwndAudioDevice,(UINT) CB_GETCURSEL, 0, 0),0);
  devinfo=(PaDeviceInfo *)Pa_GetDeviceInfo(params.device);
//  params.channelCount=devinfo->maxInputChannels;
  params.channelCount=2; // need to modify realloc in order to change this
  params.sampleFormat=paInt32; // need to modify realloc in order to change this
  params.suggestedLatency=devinfo->defaultLowInputLatency;
  params.hostApiSpecificStreamInfo=NULL;

  if (Pa_OpenStream(&audioStream, &params, NULL, 48000, paFramesPerBufferUnspecified, paNoFlag, NULL, NULL) != paNoError || Pa_StartStream(audioStream)!=paNoError) { DBGINT;
    Alert("Error opening audio stream");
    SafeShutdown();
  }
  while (frameBufferSize < 48000*5*1*2) { DBGINT;
    inputFrames=Pa_GetStreamReadAvailable(audioStream);
    if (inputFrames==paInputOverflowed) { DBGINT;
      Alert("input overflowed");
      SafeShutdown();
    }
    if (inputFrames>0) { DBGINT;
//      frameBuffer=realloc(frameBuffer,frameBufferSize+inputFrames*4*2); // inputFrames frames, 4 bytes per frame (paInt32), 2 channels per frame
      if (Pa_ReadStream(audioStream,frameBuffer+frameBufferSize,inputFrames)!=paNoError) { DBGINT;
        Alert("Error reading audio stream");
        SafeShutdown();
      }
      frameBufferSize+=inputFrames*4*2;
    }
  }
  Pa_StopStream(audioStream);
  Pa_CloseStream(audioStream);

  params.device=Pa_GetDefaultOutputDevice();
  devinfo=(PaDeviceInfo *)Pa_GetDeviceInfo(params.device);
Alert(devinfo->name);
  params.channelCount=2;
  params.sampleFormat=paInt32;
  params.suggestedLatency=devinfo->defaultLowOutputLatency;
  params.hostApiSpecificStreamInfo=NULL;

  if (Pa_OpenStream(&audioStream, NULL, &params, 48000, paFramesPerBufferUnspecified, paNoFlag, audioOutputCallback, NULL) != paNoError || Pa_StartStream(audioStream)!=paNoError) { DBGINT;
    Alert("Error opening audio stream");
    SafeShutdown();
  }
Sleep(2000);
  streaminfo=(PaStreamInfo *)Pa_GetStreamInfo(audioStream);
  Alert("%f %f %f", streaminfo->inputLatency, streaminfo->outputLatency, streaminfo->sampleRate);
  Pa_StopStream(audioStream);
  Pa_CloseStream(audioStream);
}
*/

double avgcrf;
int totalABytes=0,totalVBytes=0;

DWORD WINAPI screen_capture_loop(LPVOID param) { DBGINT;
  BYTE* tData;
  int i,j,k,bandwidth_remaining=0;
  int totalFPS=0,totalTime=0,fullTime=0;
  int cStart;
  datablock *capturedFrame;
  x264_nal_t *nals;
  char tbuf[500];
  clock_t a;
  const char *alabel="Encrypt & Transmit Frame";
  const char *blabel="Sleep";

//int tStart,tEnd,tTot=0,tCmp[10]={0};
//char tbBuf[500];

  get_x264_stream_input(&hDCScreen, &hDCScreenCopyBuffer, &aDesktop, &aBitmap, &desktopBitmapData);

  get_CELT_stream_input();

  clientTimeInit=FALSE;
  timesync();

  bStarted=FALSE;
//tStart=clock();
  while (clientActive) { DBGINT;
//tbBuf[0]=0;
//tEnd=clock(); sprintf(tbBuf+strlen(tbBuf), "%0.2f ", 1.0*(tCmp[0]+=tEnd-tStart)/tTot ); tTot+=tEnd-tStart; tStart=tEnd;
    if (fullTime==0)
      fullTime=clock();
    cStart=clock();

    EnterCriticalSection(&encoderlock);
    capturedFrame=screen_capture_frame();
    nals=(x264_nal_t *)capturedFrame->data;

    benchmark_range(alabel,BM_START,&a);

    i=clock();
    totalTime+=i-cStart;
//tEnd=clock(); sprintf(tbBuf+strlen(tbBuf), "%0.2f ", 1.0*(tCmp[1]+=tEnd-tStart)/tTot ); tTot+=tEnd-tStart; tStart=tEnd;
    checkCursor();
    if (transmit_bulk(clientsocket,nals[0].p_payload,capturedFrame->size, TRANSMIT_VIDEO) == SOCKET_ERROR) { DBGINT; // do not count the time it takes to send a frame in the potential fps, since this function can block
      LeaveCriticalSection(&encoderlock);
      break;
    }

//tEnd=clock(); sprintf(tbBuf+strlen(tbBuf), "%0.2f ", 1.0*(tCmp[2]+=tEnd-tStart)/tTot ); tTot+=tEnd-tStart; tStart=tEnd;
    cStart=clock();
    j=capturedFrame->size;
    LeaveCriticalSection(&encoderlock);

    benchmark_range(alabel,BM_STOP,&a);

    i=bandwidth_goal - j; // remaining bytes that can be sent at this time

    if (i<0) {
      bandwidth_remaining+=i;
    }
    else if (bandwidth_remaining<0) {
      bandwidth_remaining=(i+=bandwidth_remaining);
    }

    if (bandwidth_remaining>0)
      bandwidth_remaining=0;

    i=LIMIT(i,0,RECV_BUFFER_SIZE);
    transferLowPriorityData(i);

    benchmark_range(blabel,BM_START,&a);

    i=clock();
    totalTime+=i-cStart;
    totalFPS++;
    if (i-fullTime>1000) { DBGINT;
      j=(max_bitrate+(opus_bitrate/8000))*(avgClientLatency==0 ? 1 : avgClientLatency);
      if (tcpThrottling)
        j=LIMIT(j,1,tcpThrottling);
      else
        j=LIMIT(j,1,MAX_SOCKET_BUFFER_SIZE);

      if (clientsocket!=SOCK_DISCONNECTED) {
        setsockopt(clientsocket, SOL_SOCKET, SO_SNDBUF, (char *)&j, sizeof(j)); // how much data to buffer before we start blocking because either the client isn't a fast enough decoder or the network is congested. this will be a congestion buffer.
        k=sizeof(j);
        getsockopt(clientsocket, SOL_SOCKET, SO_SNDBUF, (char *)&j, &k);
      }

      sprintf(tbuf,"%0.2f fps [%0.2f potential] CRF %.2f Total: %.2fkb\r\nV. Bitrate %.2fkb/sec\tA. Bitrate %.2fkb/sec\tSend Window %i/%i\r\nBenchmark Data:\r\n",totalFPS * 1000.0 / (i-fullTime),totalFPS * 1000.0 / totalTime,avgcrf/totalFPS,totalSent/1000.0,1.0*totalVBytes/(i-fullTime), 1.0*totalABytes/(i-fullTime), (max_bitrate+(opus_bitrate/8000))*(avgClientLatency==0 ? 1 : avgClientLatency), j);
      benchmark_string(tbuf+strlen(tbuf));
      sprintf(tbuf+strlen(tbuf), "\r\nCTO: %i±%i\r\nDelay to client display: %ims",abs(ctoLow+ctoHigh)/2,(ctoHigh-ctoLow)/2,clientLatency);
      sprintf(tbuf+strlen(tbuf), "\r\nClock: %i", clock_w());
      SetWindowText(hwndText,tbuf);
      EnterCriticalSection(&byteslock);
      avgcrf=totalTime=totalFPS=totalVBytes=totalABytes=0;
      LeaveCriticalSection(&byteslock);
      fullTime=i;
      timesync();
    }
    if (totalFPS * 1000.0 / (i-fullTime) > fps_goal && limit_frame_capture)
      Sleep(totalFPS*1000.0/fps_goal+fullTime-i);

    benchmark_range(blabel,BM_STOP,&a);
//tEnd=clock(); sprintf(tbBuf+strlen(tbBuf), "%0.2f ", 1.0*(tCmp[3]+=tEnd-tStart)/tTot ); tTot+=tEnd-tStart; tStart=tEnd;
//Debug("%s",tbBuf);
  }
  clientActive=FALSE;
  release_allocated_variables();
  release_audio_variables();
  release_transfer_variables();
  if (sslEngaged) { DBGINT;
    LeaveCriticalSection(&scalestartlock);
    sslEngaged=FALSE;
  }
  ExitThread(1);
}

IDataObject *createDataObject();
IDropSource *createDropSource();

HCURSOR defaultCursor;

datalist *file_list=NULL;
datalist *file_end=NULL;
int file_counter=0;

void deleteFileList(datalist **flist) {
  fileInfo *fInfo;
  datalist *dlist;
  while (*flist) {
    fInfo=((fileInfo*)dlist_dataptr(*flist));
    if (fInfo->name) free(fInfo->name);
    if (fInfo->file) CloseHandle(fInfo->file);
    if (fInfo->event1) CloseHandle(fInfo->event1);
    if (fInfo->event2) CloseHandle(fInfo->event2);
    dlist=*flist;
    *flist=(*flist)->next;
    free(dlist);
  }
  file_counter=0;
}

void CALLBACK doDragDrop(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime) {
  INPUT input;
  DWORD k;
  IDataObject *dObj;
  IDropSource *dSrc;
  POINT mousepos;
  cmdStruct cmd;

  KillTimer(hwnd,idEvent);

  EnterCriticalSection(&mouselock);
  BlockInput(TRUE);

  GetCursorPos(&mousepos);

  tmpWindow = CreateWindowEx(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, classname, "S", WS_POPUP | WS_VISIBLE, mousepos.x-1, mousepos.y-1, 3, 3, NULL, NULL, hInst, NULL);

  SetFocus(tmpWindow);

  Sleep(1);

  input.type=INPUT_MOUSE;
  input.mi.dx=mousepos.x;
  input.mi.dy=mousepos.y;
  input.mi.mouseData=0;
  input.mi.time=0;
  input.mi.dwExtraInfo=0;
  input.mi.dwFlags=MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_LEFTDOWN;
  SendInput(1,&input,sizeof(INPUT));

  BlockInput(FALSE);
  LeaveCriticalSection(&mouselock);

  dragging=TRUE;
  DoDragDrop(dObj=createDataObject(),dSrc=createDropSource(),DROPEFFECT_COPY,&k);
  dragging=FALSE;
  dObj->lpVtbl->Release(dObj);
  dSrc->lpVtbl->Release(dSrc);
  DestroyWindow(tmpWindow);
  tmpWindow=NULL;

  EnterCriticalSection(&flistlock);
  deleteFileList(&file_list);
  LeaveCriticalSection(&flistlock);

  cmd.cmd=CMD_DRAGDROP;
  cmd.param1=CMD_DRAGDROP_COMPLETE;
  transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);
}

LRESULT CALLBACK wndMsgProcessor (HWND hwnd, UINT nMsg, WPARAM wParam, LPARAM lParam) { DBGINT;
  D3DPRESENT_PARAMETERS presParams;
  PAINTSTRUCT ps;
  POINT mp;
  BITMAPINFO bi;
  BITMAP bmp;
  int i,j;
  BOOL reconfigure=FALSE;
  char tbuf[RECV_BUFFER_SIZE];
  DWORD IP;
  cmdStruct cmd;
  x264_param_t params;
  HGLOBAL memHandle;
  char *memPtr;
  datalist *dlist;
  HDC dc;

  if (nMsg != WM_DESTROY && nMsg!=WM_CLOSE && hwnd!=mainWindow) {
    switch (nMsg)
    {
    case WM_PAINT:
      dc=BeginPaint(hwnd, &ps);
//      FillRect(dc, &ps.rcPaint, (HBRUSH) (COLOR_WINDOW));
      EndPaint(hwnd, &ps);
      break;
    default:
      return DefWindowProc(hwnd, nMsg, wParam, lParam);
      break;
    }
  }
  else switch (nMsg) { DBGINT;
    case WM_DISPLAYCHANGE:
      if (hDCScreen!=NULL) {
        EnterCriticalSection(&encoderlock);
        release_allocated_variables();
        get_x264_stream_input(&hDCScreen, &hDCScreenCopyBuffer, &aDesktop, &aBitmap, &desktopBitmapData);
/*
        ocmd.cmd=CMD_RESTARTSTREAM;
        ocmd.param1=wWidth;
        ocmd.param2=wHeight;
        transmit(clientsocket,&ocmd,sizeof(ocmd),TRANSMIT_CMD);
*/
        LeaveCriticalSection(&encoderlock);
      }
      break;
    case TASKTRAY_MESSAGE:
      switch(lParam) { DBGINT;
        case WM_LBUTTONDBLCLK:
          ShowWindow(mainWindow, SW_SHOW);
          ShowWindow(mainWindow, SW_RESTORE);
          break;
      }
      break;
    case WM_CHANGECBCHAIN:
      if (nextClipViewer) { DBGINT;
        if (nextClipViewer==(HWND)wParam)
          nextClipViewer=(HWND)lParam;
        else
          SendMessage(nextClipViewer, nMsg, wParam, lParam);
      }
      return 0;

    case WM_DRAWCLIPBOARD: // clipboard changed
      if (ignoreClip)
        ignoreClip=FALSE;
      else if (clientsocket!=SOCK_DISCONNECTED && OpenClipboard(mainWindow)) { DBGINT;
        ((cmdStruct *)tbuf)->cmd=CMD_CLIPBOARD;
        ((cmdStruct *)tbuf)->param1=0;
        transmit_bulk(clientsocket,tbuf,sizeof(cmdStruct),TRANSMIT_CMD);
        #define SEND_TEXT_CLIPBOARD_FORMAT(formatvar,sendvar) \
        if (IsClipboardFormatAvailable(formatvar) && (memHandle=GetClipboardData(formatvar))) { DBGINT; \
          if (memPtr=GlobalLock(memHandle)) { DBGINT; \
            j=GlobalSize(memHandle); \
            ((cmdStruct *)tbuf)->cmd=CMD_CLIPBOARD; \
            ((cmdStruct *)tbuf)->param1=sendvar; \
            for (i=0; i<j; i+=RECV_BUFFER_SIZE-sizeof(cmdStruct)) { DBGINT; \
              ((cmdStruct *)tbuf)->param2=(i==0 ? j : 0); \
              memcpy(((BYTE *)tbuf)+sizeof(cmdStruct),memPtr+i,LIMIT(j-i,0,RECV_BUFFER_SIZE-sizeof(cmdStruct))); \
              transmit_bulk(clientsocket,tbuf,sizeof(cmdStruct)+LIMIT(j-i,0,RECV_BUFFER_SIZE-sizeof(cmdStruct)),TRANSMIT_CMD); \
            } \
            GlobalUnlock(memHandle); \
          } \
        }
        SEND_TEXT_CLIPBOARD_FORMAT(CF_TEXT,CF_TEXT);
        SEND_TEXT_CLIPBOARD_FORMAT(CF_HTML,TRANSMIT_CF_HTML);
        SEND_TEXT_CLIPBOARD_FORMAT(CF_RTF,TRANSMIT_CF_RTF);

        if (IsClipboardFormatAvailable(CF_BITMAP) && (memHandle=GetClipboardData(CF_BITMAP))) { DBGINT;
          if (!GetObject((HBITMAP)memHandle, sizeof(BITMAP), (LPSTR)&bmp)) { Alert("Couldn't GetObject"); SafeShutdown(); }
          j=sizeof(datalist)+sizeof(BITMAPINFOHEADER)+bmp.bmWidth*bmp.bmHeight*32/8;
          memPtr=malloc(j);
          bi.bmiHeader.biSize = sizeof(BITMAPINFO);    
          bi.bmiHeader.biWidth = bmp.bmWidth;
          bi.bmiHeader.biHeight = bmp.bmHeight;  
          bi.bmiHeader.biPlanes = 1;    
          bi.bmiHeader.biBitCount = 32;    
          bi.bmiHeader.biCompression = BI_RGB;    
          bi.bmiHeader.biSizeImage = 0;  
          bi.bmiHeader.biXPelsPerMeter = 0;    
          bi.bmiHeader.biYPelsPerMeter = 0;    
          bi.bmiHeader.biClrUsed = 0;    
          bi.bmiHeader.biClrImportant = 0;
          GetDIBits(mainhDC, (HBITMAP)memHandle, 0, bmp.bmHeight, memPtr+sizeof(datalist)+sizeof(BITMAPINFOHEADER), &bi, DIB_RGB_COLORS);
          memcpy(memPtr+sizeof(datalist),&(bi.bmiHeader),sizeof(BITMAPINFOHEADER));
          dlist=(datalist *)memPtr;
          dlist->size=j-sizeof(datalist);
          dlist->offset=0;
          dlist->type=DATATYPE_CLIPBOARD_BITMAP;
          dlist->next=NULL;
          EnterCriticalSection(&transferlock);
            if (!transferList)
              transferList=dlist;
            else {
              dlist=transferList;
              while (dlist->next)
                dlist=dlist->next;
              dlist->next=memPtr;
            }
          LeaveCriticalSection(&transferlock);

/*
          ((cmdStruct *)tbuf)->cmd=CMD_CLIPBOARD;
          ((cmdStruct *)tbuf)->param1=CF_BITMAP;
          for (i=0; i<j; i+=RECV_BUFFER_SIZE-sizeof(cmdStruct)) { DBGINT;
            ((cmdStruct *)tbuf)->param2=(i==0 ? j : 0);
            memcpy(((BYTE *)tbuf)+sizeof(cmdStruct),memPtr+sizeof(datalist)+i,LIMIT(j-i,0,RECV_BUFFER_SIZE-sizeof(cmdStruct)));
            transmit_bulk(clientsocket,tbuf,sizeof(cmdStruct)+LIMIT(j-i,0,RECV_BUFFER_SIZE-sizeof(cmdStruct)),TRANSMIT_CMD);
          }
          free(memPtr);
*/
        }
        CloseClipboard();
      }
      if (nextClipViewer)
        SendMessage(nextClipViewer, nMsg, wParam, lParam);
      return 0;
    case WM_CLOSE:
    case WM_DESTROY:
      if (hwnd!=mainWindow) return 0;

      ChangeClipboardChain(mainWindow,nextClipViewer);
      DeleteObject(mFont);
      RevokeDragDrop(mainWindow);
      dropTarget->lpVtbl->Release(dropTarget);
      mainWindow=NULL;
      PostQuitMessage (0);
      break;
    case WM_PAINT:
      EnterCriticalSection(&paintwinlock);
      BeginPaint(hwnd, &ps);
      if (avCodecContext==NULL) { DBGINT;
        FillRect(mainhDC, &ps.rcPaint, (HBRUSH) (COLOR_WINDOW));
      } else {
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);    
        bi.bmiHeader.biWidth = wWidth;    
        bi.bmiHeader.biHeight = wHeight;  
        bi.bmiHeader.biPlanes = 1;    
        bi.bmiHeader.biBitCount = 32;    
        bi.bmiHeader.biCompression = BI_RGB;    
        bi.bmiHeader.biSizeImage = 0;  
        bi.bmiHeader.biXPelsPerMeter = 0;    
        bi.bmiHeader.biYPelsPerMeter = 0;    
        bi.bmiHeader.biClrUsed = 0;    
        bi.bmiHeader.biClrImportant = 0;
        FillRect(mainhDC, &ps.rcPaint, (HBRUSH) (COLOR_WINDOW));
        SetDIBitsToDevice(mainhDC,0,0,wWidth,wHeight,0,0,0,wHeight,inputBitmapData,&bi,DIB_RGB_COLORS);
      }
      EndPaint(hwnd, &ps);
      LeaveCriticalSection(&paintwinlock);
      return 1;
    case WM_SIZE:
      if (serverActive && wParam==SIZE_MINIMIZED) { DBGINT;
        ShowWindow(mainWindow,SW_HIDE);
      }
      if (!serverResized && !serverActive && clientActive) { DBGINT; // we are the client
        sizeTimer=clock();
        winX=LOWORD(lParam);
        winY=HIWORD(lParam);
        SetTimer(mainWindow, 1234652, 251, NULL);
      }

      if (serverResized) serverResized=FALSE;

      EnterCriticalSection(&paintwinlock);
      if (d3dDevice!=NULL) { DBGINT;
          ZeroMemory(&presParams,sizeof(presParams));
          presParams.Windowed=TRUE;
          presParams.SwapEffect=D3DSWAPEFFECT_FLIP;
          presParams.BackBufferFormat=d3ddm.Format;
          presParams.BackBufferWidth=LOWORD(lParam);
          presParams.BackBufferHeight=HIWORD(lParam);
          presParams.BackBufferCount=1;
          presParams.hDeviceWindow=mainWindow;
          presParams.PresentationInterval=D3DPRESENT_INTERVAL_ONE;
          IDirect3DDevice9_Reset(d3dDevice, &presParams);
      }
      LeaveCriticalSection(&paintwinlock);
      break;
    case WM_COMMAND:
      if (HIWORD(wParam)==BN_CLICKED) { DBGINT;
        if ((HWND)lParam == hwndListen) { DBGINT;
          if (serverActive) { DBGINT;
            EnableWindow(hwndListen, FALSE);
            serverActive=FALSE;
            if (clientsocket!=SOCK_DISCONNECTED) { DBGINT;
              closesocket(clientsocket);
              clientsocket=SOCK_DISCONNECTED;
            }
          } else {
            if (keypair==NULL) { DBGINT;
              QueueUserAPC(init_gcrypt, crypt_thread, (ULONG_PTR)NULL);
            }
            EnableWindow(hwndListen, FALSE);
            EnableWindow(hwndConnect, FALSE);
            EnableWindow(hwndIP, FALSE);
            start_server();
          }
        }
        if ((HWND)lParam == hwndConnect) { DBGINT;
          if (clientActive) { DBGINT;
            EnableWindow(hwndConnect, FALSE);

            clientActive=FALSE;
            if (clientsocket!=SOCK_DISCONNECTED) { DBGINT;
              closesocket(clientsocket);
              clientsocket=SOCK_DISCONNECTED;
            }
          } else {
            SetFocus(mainWindow);
//            SendMessage(hwndIP, IPM_GETADDRESS, 0, (LPARAM) &IP);
//            sprintf(tbuf, "%i.%i.%i.%i", FIRST_IPADDRESS(IP), SECOND_IPADDRESS(IP), THIRD_IPADDRESS(IP), FOURTH_IPADDRESS(IP));
            EnableWindow(hwndListen, FALSE);
            EnableWindow(hwndConnect, FALSE);
            start_client();
          }
        }
      }
      if ((HWND)lParam == hwndAudioDevice && HIWORD(wParam)==CBN_SELCHANGE) { DBGINT;
        i=SendMessage(hwndAudioDevice,(UINT) CB_GETITEMDATA,(WPARAM) ComboBox_GetCurSel((HWND)lParam), 0);
        if ((INT8)LOBYTE(HIWORD(i))!=-100 && (INT8)HIBYTE(HIWORD(i))!=-100)
          setMuxInput(LOBYTE(LOWORD(i)), LOBYTE(HIWORD(i)), HIBYTE(HIWORD(i)), HIBYTE(LOWORD(i)));
        if (waveIn!=NULL) { DBGINT; // restart the stream (in case we changed audio device and need to reselect waveIn device)
          release_audio_variables();
          get_CELT_stream_input();
        }
      }
      if ((HWND)lParam == hwndACB && HIWORD(wParam)==BN_CLICKED) { DBGINT;
        if (Button_GetCheck((HWND)lParam)==BST_CHECKED) { DBGINT;
          audio_enabled=TRUE;
          if (waveIn!=NULL)
            if (waveInStart(waveIn)!=MMSYSERR_NOERROR) { DBGINT; 
              Alert("Could not start wave capture.");
              SafeShutdown();
            }
        } else {
          audio_enabled=FALSE;
          if (waveIn!=NULL)
            if (waveInStop(waveIn)!=MMSYSERR_NOERROR) { DBGINT; 
              Alert("Could not stop wave capture.");
              SafeShutdown();
            }
        }
      }
      if ((HWND)lParam == hwndABL && (HIWORD(wParam)==CBN_SELCHANGE || HIWORD(wParam)==CBN_KILLFOCUS)) { DBGINT;
        GetWindowText((HWND)lParam,tbuf,50);
        i=atoi(tbuf);
        i=LIMIT(i,20,800);
        sprintf(tbuf, "%ims", i);
        SetWindowText((HWND)lParam,tbuf);
        GetWindowText(hwndABM,tbuf,50);
        cmd.cmd=CMD_REBUFFER;
        cmd.param1=i;

        if (atoi(tbuf)-i<60) { DBGINT;
          cmd.param2=i+60;
          sprintf(tbuf, "%ims", i+60);
          SetWindowText(hwndABM,tbuf);
        } else {
          GetWindowText(hwndABM,tbuf,50);
          i=atoi(tbuf);
          i=LIMIT(i,80,860);
          cmd.param2=i;
        }
        transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);
      }
      if ((HWND)lParam == hwndABM && (HIWORD(wParam)==CBN_SELCHANGE || HIWORD(wParam)==CBN_KILLFOCUS)) { DBGINT;
        GetWindowText((HWND)lParam,tbuf,50);
        i=atoi(tbuf);
        i=LIMIT(i,80,860);
        sprintf(tbuf, "%ims", i);
        SetWindowText((HWND)lParam,tbuf);
        GetWindowText(hwndABL,tbuf,50);

        cmd.cmd=CMD_REBUFFER;
        cmd.param2=i;

        if (i-atoi(tbuf)<60) { DBGINT;
          cmd.param1=i-60;
          sprintf(tbuf, "%ims", i-60);
          SetWindowText(hwndABL,tbuf);
        } else {
          GetWindowText(hwndABL,tbuf,50);
          i=atoi(tbuf);
          i=LIMIT(i,20,800);
          cmd.param1=i;
        }
        transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);
      }
      if ((HWND)lParam == hwndAC && (HIWORD(wParam)==CBN_SELCHANGE || HIWORD(wParam)==CBN_KILLFOCUS)) { DBGINT;
        GetWindowText((HWND)lParam,tbuf,50);
        i=atoi(tbuf);
        i=LIMIT(i,0,10);
        sprintf(tbuf, "%i", i);
        SetWindowText((HWND)lParam,tbuf);
        opus_complexity=i;
        if (aenc)
          opus_encoder_ctl(aenc, OPUS_SET_COMPLEXITY(opus_complexity));
      }
      if ((HWND)lParam == hwndABITRATE && (HIWORD(wParam)==CBN_SELCHANGE || HIWORD(wParam)==CBN_KILLFOCUS)) { DBGINT;
        GetWindowText((HWND)lParam,tbuf,50);
        i=atoi(tbuf);
        i=LIMIT(i,1,64);
        sprintf(tbuf, "%i kbytes/s", i);
        SetWindowText((HWND)lParam,tbuf);
        opus_bitrate=i*8000;
        if (aenc)
          opus_encoder_ctl(aenc, OPUS_SET_BITRATE(opus_bitrate));
      }
      if ((HWND)lParam == hwndSCALE && HIWORD(wParam)==CBN_SELCHANGE) { DBGINT;
        EnterCriticalSection(&encoderlock);
        i=scale;
        switch (ComboBox_GetCurSel((HWND)lParam)) { DBGINT;
          case 0: scale=FALSE; break;
          case 1: scale_mode=SWS_POINT; scale=TRUE; break;
          case 2: scale_mode=SWS_FAST_BILINEAR; scale=TRUE; break;
          case 3: scale_mode=SWS_BILINEAR; scale=TRUE; break;
          case 4: scale_mode=SWS_BICUBLIN; scale=TRUE; break;
          case 5: scale_mode=SWS_BICUBIC; scale=TRUE; break;
          case 6: scale_mode=SWS_SPLINE; scale=TRUE; break;
          case 7: scale_mode=SWS_SINC; scale=TRUE; break;
        }
        if (serverActive && x264encoder) { DBGINT;
          if ((i==FALSE && scale) || (i!=FALSE && !scale)) { DBGINT;
            release_allocated_variables();
            get_x264_stream_input(&hDCScreen, &hDCScreenCopyBuffer, &aDesktop, &aBitmap, &desktopBitmapData);
            cmd.cmd=CMD_RESTARTSTREAM;
            cmd.param1=dWidth;
            cmd.param2=dHeight;
            transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);
          }
          reinitscaling=TRUE;
        }
        LeaveCriticalSection(&encoderlock);
        return 0;
      }

      if ((HWND)lParam == hwndCRF || (HWND)lParam == hwndFPS || (HWND)lParam == hwndBITRATE || (HWND)lParam == hwndSPEED || ((HWND)lParam == hwndFD && HIWORD(wParam)==BN_CLICKED)) { DBGINT;
        if (HIWORD(wParam)==CBN_KILLFOCUS || ((HWND)lParam == hwndFD)) { DBGINT;
          if ((HWND)lParam == hwndFPS) { DBGINT;
            reconfigure=FALSE;
            GetWindowText((HWND)lParam,tbuf,50);
            i=atoi(tbuf);
            i=LIMIT(i,1,120);
            sprintf(tbuf, "%i", i);
            SetWindowText((HWND)lParam,tbuf);
            if (i==fps_goal) return 0;
            fps_goal=i;
            bandwidth_goal=max_bitrate*1000/fps_goal;
            cmd.cmd=CMD_BANDWIDTH_GOAL;
            cmd.param1=bandwidth_goal;
            cmd.param2=fps_goal;
            transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);
          }
          if ((HWND)lParam == hwndCRF) { DBGINT;
            reconfigure=TRUE;
            GetWindowText((HWND)lParam,tbuf,50);
            i=atoi(tbuf);
            i=LIMIT(i,1,60);
            sprintf(tbuf, "%i", i);
            SetWindowText((HWND)lParam,tbuf);
            if (i==crf_goal) return 0;
            crf_goal=i;
          }
          if ((HWND)lParam == hwndBITRATE) { DBGINT;
            reconfigure=TRUE;
            GetWindowText((HWND)lParam,tbuf,50);
            i=atoi(tbuf);
            i=LIMIT(i,100,10000);
            sprintf(tbuf, "%i kbytes/s", i);
            SetWindowText((HWND)lParam,tbuf);
            if (i==max_bitrate) return 0;
            max_bitrate=i;
            bandwidth_goal=max_bitrate*1000/fps_goal;
            cmd.cmd=CMD_BANDWIDTH_GOAL;
            cmd.param1=bandwidth_goal;
            cmd.param2=fps_goal;
            transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);
          }
          if ((HWND)lParam == hwndFD) { DBGINT;
            if (Button_GetCheck((HWND)lParam)==BST_CHECKED) { DBGINT;
              fast_decode=TRUE;
            } else {
              fast_decode=FALSE;
            }
          }

          if (serverActive && x264encoder) { DBGINT;
            EnterCriticalSection(&encoderlock);
            if (!reconfigure) { DBGINT; // if fps goal changed we need to restart the stream
              release_allocated_variables();
              get_x264_stream_input(&hDCScreen, &hDCScreenCopyBuffer, &aDesktop, &aBitmap, &desktopBitmapData);
              cmd.cmd=CMD_RESTARTSTREAM;
              cmd.param1=wWidth;
              cmd.param2=wHeight;
              transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);
            } else { // otherwise, options are amenable to reconfiguration
              x264_encoder_parameters(x264encoder, &params);
              params.rc.i_vbv_max_bitrate=max_bitrate*8;
              params.rc.i_vbv_buffer_size=max_bitrate*8/params.i_fps_num;
              params.rc.f_rf_constant = crf_goal;
              if (x264_encoder_reconfig(x264encoder, &params)) { DBGINT;
                Alert("Reconfigure failed.");
              }
            }
            LeaveCriticalSection(&encoderlock);
            return 0;
          }
        }
      }
      break;
    case WM_SYSCOMMAND:
      if (wParam==MENU_DISCONNECT) { DBGINT;
        if (clientsocket!=SOCK_DISCONNECTED) { DBGINT;
          closesocket(clientsocket);
          clientsocket=SOCK_DISCONNECTED;
        }
        return 0;
      }
      else if (wParam==MENU_PASSWORD) { DBGINT;
        DialogBox(hInst, MAKEINTRESOURCE(IDD_PASSWORD), mainWindow, PasswordProc);
        break;
      }
      break;
    case WM_MOUSEMOVE:
      if (!serverActive && clientActive) { DBGINT; // we are the client
        if (!captureMouseMove) { DBGINT;
          captureMouseMove=TRUE;
          return 0;
        }
        cmd.cmd=CMD_MOUSEMOVE;
        cmd.param1=LOWORD(lParam);
        cmd.param2=HIWORD(lParam);
        transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);
      }
      return 0;
      break;
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK:
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
      if (!serverActive && clientActive) { DBGINT; // we are the client
        cmd.cmd=CMD_MOUSECLICK;
        cmd.param1=MAKELONG(GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam));
        switch (nMsg) { DBGINT;
          case WM_LBUTTONDOWN:
          case WM_LBUTTONDBLCLK: cmd.param2=MAKELONG(CMD_MOUSECLICK_LBUTTON,CMD_MOUSECLICK_DOWN); break;
          case WM_RBUTTONDOWN:
          case WM_RBUTTONDBLCLK: cmd.param2=MAKELONG(CMD_MOUSECLICK_RBUTTON,CMD_MOUSECLICK_DOWN); break;
          case WM_MBUTTONDOWN:
          case WM_MBUTTONDBLCLK: cmd.param2=MAKELONG(CMD_MOUSECLICK_MBUTTON,CMD_MOUSECLICK_DOWN); break;
          case WM_LBUTTONUP: cmd.param2=MAKELONG(CMD_MOUSECLICK_LBUTTON,CMD_MOUSECLICK_UP); break;
          case WM_RBUTTONUP: cmd.param2=MAKELONG(CMD_MOUSECLICK_RBUTTON,CMD_MOUSECLICK_UP); break;
          case WM_MBUTTONUP: cmd.param2=MAKELONG(CMD_MOUSECLICK_MBUTTON,CMD_MOUSECLICK_UP); break;
          case WM_MOUSEWHEEL:
            mp.x=GET_X_LPARAM(lParam);
            mp.y=GET_Y_LPARAM(lParam);
            ScreenToClient(mainWindow,&mp);
            cmd.param1=MAKELONG(mp.x,mp.y);
            cmd.param2=MAKELONG(CMD_MOUSECLICK_WHEEL,HIWORD(wParam));
            break;
        }
        transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);
      }
      return 0;
      break;
   default:
      return 0;
//DLG      DefWindowProc (hwnd, nMsg, wParam, lParam);
  }
  return 0;
}

void initialize_directx_capture(void) { DBGINT;
  D3DPRESENT_PARAMETERS presParams;

  if (!DXObj && ((DXObj=Direct3DCreate9(D3D_SDK_VERSION))==NULL)) { DBGINT;
    MsgBox("Could not create DX9 object.");
    SafeShutdown();
  }

  IDirect3D9_GetAdapterDisplayMode(DXObj, D3DADAPTER_DEFAULT, &d3ddm );

  ZeroMemory(&presParams,sizeof(presParams));
  presParams.Windowed=FALSE;
//  presParams.SwapEffect=D3DSWAPEFFECT_DISCARD;
  presParams.SwapEffect=D3DSWAPEFFECT_FLIP;
  presParams.BackBufferFormat=d3ddm.Format;
  presParams.BackBufferWidth=dWidth;
  presParams.BackBufferHeight=dHeight;
  presParams.BackBufferCount=1;
  presParams.hDeviceWindow=GetDesktopWindow();
  presParams.PresentationInterval=D3DPRESENT_INTERVAL_ONE;
  presParams.Flags=D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
  #define D3DCREATE_NOWINDOWCHANGES 0x00000800L

  switch (IDirect3D9_CreateDevice(DXObj, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, GetDesktopWindow(), D3DCREATE_MULTITHREADED | D3DCREATE_HARDWARE_VERTEXPROCESSING, &presParams, &d3dcapturedevice)) { DBGINT;
    case D3D_OK:
      break;
    case D3DERR_DEVICELOST:
      MsgBox("Could not create DX9 device 1: device lost.");
      SafeShutdown();
      break;
    case D3DERR_INVALIDCALL:
      MsgBox("Could not create DX9 device 1: invalid call.");
      SafeShutdown();
      break;
    case D3DERR_NOTAVAILABLE:
      MsgBox("Could not create DX9 device 1: not available.");
      SafeShutdown();
      break;
    case D3DERR_OUTOFVIDEOMEMORY:
      MsgBox("Could not create DX9 device 1: out of memory.");
      SafeShutdown();
      break;
  }

//  errcheck(IDirect3DDevice9_Present(d3dcapturedevice, NULL, NULL, NULL, NULL));
}

void initialize_directx_output(void) { DBGINT;
  D3DPRESENT_PARAMETERS presParams,presParams2;

  if ((DXObj=Direct3DCreate9(D3D_SDK_VERSION))==NULL) { DBGINT;
    MsgBox("Could not create DX9 object.");
    SafeShutdown();
  }

  IDirect3D9_GetAdapterDisplayMode(DXObj, D3DADAPTER_DEFAULT, &d3ddm );

  ZeroMemory(&presParams,sizeof(presParams));
  presParams.Windowed=TRUE;
  presParams.SwapEffect=D3DSWAPEFFECT_DISCARD;
  presParams.BackBufferFormat=d3ddm.Format;
  presParams.BackBufferWidth=dWidth;
  presParams.BackBufferHeight=dHeight;
  presParams.BackBufferCount=1;
  presParams.hDeviceWindow=mainWindow;
  presParams.PresentationInterval=D3DPRESENT_INTERVAL_ONE;

  switch (IDirect3D9_CreateDevice(DXObj, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, mainWindow, D3DCREATE_MULTITHREADED | D3DCREATE_HARDWARE_VERTEXPROCESSING, &presParams, &d3dDevice)) { DBGINT;
    case D3D_OK:
      break;
    case D3DERR_DEVICELOST:
      MsgBox("Could not create DX9 device 1: device lost.");
      SafeShutdown();
      break;
    case D3DERR_INVALIDCALL:
      MsgBox("Could not create DX9 device 1: invalid call.");
      SafeShutdown();
      break;
    case D3DERR_NOTAVAILABLE:
      MsgBox("Could not create DX9 device 1: not available.");
      SafeShutdown();
      break;
    case D3DERR_OUTOFVIDEOMEMORY:
      MsgBox("Could not create DX9 device 1: out of memory.");
      SafeShutdown();
      break;
  }

  ZeroMemory(&presParams2,sizeof(presParams2));
  presParams2.Windowed=TRUE;
  presParams2.SwapEffect=D3DSWAPEFFECT_DISCARD;
  presParams2.BackBufferFormat=d3ddm.Format;
  presParams.BackBufferWidth=dWidth;
  presParams.BackBufferHeight=dHeight;
  presParams.BackBufferCount=1;
  presParams2.hDeviceWindow=GetDesktopWindow();
  presParams2.PresentationInterval=D3DPRESENT_INTERVAL_ONE;
  presParams2.Flags=D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
  #define D3DCREATE_NOWINDOWCHANGES 0x00000800L

  switch (IDirect3D9_CreateDevice(DXObj, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, GetDesktopWindow(), D3DCREATE_MULTITHREADED | D3DCREATE_HARDWARE_VERTEXPROCESSING, &presParams2, &d3dDevice2)) { DBGINT;
    case D3D_OK:
      break;
    case D3DERR_DEVICELOST:
      MsgBox("Could not create DX9 device 2: device lost.");
      SafeShutdown();
      break;
    case D3DERR_INVALIDCALL:
      MsgBox("Could not create DX9 device 2: invalid call.");
      SafeShutdown();
      break;
    case D3DERR_NOTAVAILABLE:
      MsgBox("Could not create DX9 device 2: not available.");
      SafeShutdown();
      break;
    case D3DERR_OUTOFVIDEOMEMORY:
      MsgBox("Could not create DX9 device 2: out of memory.");
      SafeShutdown();
      break;
  }

}

void initialize_gdi_output(void) { DBGINT;
  BITMAPINFO   bi;

  hdcWindowPrebuffer = CreateCompatibleDC (mainhDC);
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);    
  bi.bmiHeader.biWidth = wWidth;    
  bi.bmiHeader.biHeight = wHeight;  
  bi.bmiHeader.biPlanes = 1;    
  bi.bmiHeader.biBitCount = 32;    
  bi.bmiHeader.biCompression = BI_RGB;    
  bi.bmiHeader.biSizeImage = 0;  
  bi.bmiHeader.biXPelsPerMeter = 0;    
  bi.bmiHeader.biYPelsPerMeter = 0;    
  bi.bmiHeader.biClrUsed = 0;    
  bi.bmiHeader.biClrImportant = 0;
  windowBitmap=CreateDIBSection(hdcWindowPrebuffer, &bi, DIB_RGB_COLORS, (void **) &windowBitmapData, NULL, 0);
  oldBitmap=SelectObject (hdcWindowPrebuffer, windowBitmap);
}

void start_x264_stream_display (void) { DBGINT;
  if (display_mode==MODE_DIRECTX)
    initialize_directx_output();
  if (display_mode==MODE_GDI)
    initialize_gdi_output();
}

void repaint_nonclient_area(void) { DBGINT;
  RedrawWindow(mainWindow, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);
}

void resetMainWindow() { DBGINT;
  RECT pos;
  GetWindowRect(mainWindow,&pos);

  MoveWindow(mainWindow,pos.left,pos.top,mwRect.right-mwRect.left,mwRect.bottom-mwRect.top,TRUE);
  SetWindowLong(mainWindow, GWL_STYLE, MAINWINDOWSTYLE);
  fullscreen=FALSE;
  repaint_nonclient_area();
}

void resizeMainWindow(int x, int y) { DBGINT;
  RECT pos,dimensions;
  dimensions.left=0;
  dimensions.right=x;
  dimensions.top=0;
  dimensions.bottom=y;

  GetWindowRect(mainWindow,&pos);
  AdjustWindowRect(&dimensions, MAINWINDOWSTYLE_PLAYER, 0);
  MoveWindow(mainWindow,pos.left,pos.top,dimensions.right-dimensions.left,dimensions.bottom-dimensions.top,TRUE);
  fullscreen=FALSE;
}

void setMuxInput(int wavInputDeviceId, int mixerControlId, int numItems, int itemId) { DBGINT;
  HMIXER mixer;
  MIXERCONTROLDETAILS mdetails;
  MIXERCONTROLDETAILS_BOOLEAN *bValue;
  int x;

  mixerOpen(&mixer,wavInputDeviceId,0,0,MIXER_OBJECTF_WAVEIN);
  mdetails.cbStruct=sizeof(mdetails);
  mdetails.dwControlID=mixerControlId;
  mdetails.cChannels=1;
  mdetails.hwndOwner=NULL;
  mdetails.cMultipleItems=numItems;
  mdetails.cbDetails=sizeof(MIXERCONTROLDETAILS_BOOLEAN);
  bValue=malloc(mdetails.cbDetails*mdetails.cMultipleItems);
  mdetails.paDetails=bValue;
  for (x=0; x<mdetails.cMultipleItems; x++) { DBGINT;
    if (x==mdetails.cMultipleItems-itemId-1)
      bValue[x].fValue=1;
    else
      bValue[x].fValue=0;
  }
  mixerSetControlDetails((HMIXEROBJ)mixer, &mdetails, MIXER_SETCONTROLDETAILSF_VALUE | MIXER_OBJECTF_HMIXER);
  free(bValue);
  mixerClose(mixer);
}

WNDCLASSEX wc;

int dragSourceAction;
#define DRAGSOURCE_CANCEL 1
#define DRAGSOURCE_CONTINUE 2
#define DRAGSOURCE_DROP 3
DWORD lastStatus=DROPEFFECT_NONE;

void addTransferList(datalist *dlist) {
  datalist *xlist;
  EnterCriticalSection(&transferlock);
    if (!transferList)
      transferList=dlist;
    else {
      xlist=transferList;
      while (xlist->next)
        xlist=xlist->next;
      xlist->next=dlist;
    }
  LeaveCriticalSection(&transferlock);
}

#define GetPIDLFolder(pida) (LPCITEMIDLIST)(((LPBYTE)pida)+(pida)->aoffset[0])
#define GetPIDLItem(pida, i) (LPCITEMIDLIST)(((LPBYTE)pida)+(pida)->aoffset[i+1])


void release_transfer_variables(void) {
  datalist *dlist;
  EnterCriticalSection(&transferlock);
  EnterCriticalSection(&flistlock);
  deleteFileList(&file_list);
  while (transferList) {
    dlist=transferList;
    transferList=transferList->next;
    free(dlist);
  }
  LeaveCriticalSection(&transferlock);
  LeaveCriticalSection(&flistlock);
}

int recurseFile(char *tbuf, int rpos);
HRESULT __stdcall DragEnter(struct IDropTarget *this, struct IDataObject *pDataObj,DWORD grfKeyState,POINTL pt,DWORD *pdwEffect) {
  IEnumFORMATETC *EnumTypes;
  FORMATETC o;
  STGMEDIUM m;
  int i,j,k,l;
  CIDA *iList;
  char tbuf[MAX_PATH];
  datalist *dlist,*xlist;

  if (serversocket!=SOCK_DISCONNECTED || clientsocket==SOCK_DISCONNECTED || file_list)
    return lastStatus=DROPEFFECT_NONE;

  *pdwEffect=DROPEFFECT_NONE;
  pDataObj->lpVtbl->EnumFormatEtc(pDataObj,DATADIR_GET, &EnumTypes);
  while (EnumTypes->lpVtbl->Next(EnumTypes,1,&o,NULL)==S_OK) {
//    GetClipboardFormatName(o.cfFormat, tbuf, 50);
//    MAlert("Type: %i %i %s", o.cfFormat, o.tymed, tbuf); // types 1 and 4 available (HGLOBAL and TYMED_ISTREAM)
    if (o.cfFormat==CF_IDLIST) {
      *pdwEffect=DROPEFFECT_COPY;
      break;
    }
  }
  EnumTypes->lpVtbl->Release(EnumTypes);

  if (*pdwEffect==DROPEFFECT_COPY) {
    o.cfFormat=CF_IDLIST;
    o.ptd=NULL;
    o.dwAspect=DVASPECT_CONTENT;
    o.lindex=-1;
    o.tymed=TYMED_HGLOBAL;
    pDataObj->lpVtbl->GetData(pDataObj, &o,&m);

    iList=GlobalLock(m.hGlobal);
    j=0;
    EnterCriticalSection(&flistlock);
      if (file_list) deleteFileList(&file_list);
      for (i=0; i<iList->cidl; i++) {
        if (!SHGetPathFromIDList((ITEMIDLIST*)GetPIDLItem(iList,i),tbuf)) continue;
        j+=recurseFile(tbuf,0);
      }
      dlist=(datalist *)malloc(sizeof(datalist)+j);
      dlist->size=j;
      dlist->offset=0;
      dlist->type=DATATYPE_FILELIST;
      dlist->next=NULL;
      xlist=file_list;
      k=sizeof(datalist);

      while (xlist) {
        *((int *)(((BYTE*)(dlist))+k))=l=strlen(((fileInfo*)dlist_dataptr(xlist))->name);
        k+=sizeof(int);
        memcpy(((BYTE*)(dlist))+k,((fileInfo*)dlist_dataptr(xlist))->name,l);
        k+=l;
        memcpy(((BYTE*)(dlist))+k,&(((fileInfo*)dlist_dataptr(xlist))->attr),sizeof(WIN32_FILE_ATTRIBUTE_DATA));
        k+=sizeof(WIN32_FILE_ATTRIBUTE_DATA);
        xlist=xlist->next;
      }
      addTransferList(dlist);
    LeaveCriticalSection(&flistlock);
    GlobalUnlock(m.hGlobal);
    ReleaseStgMedium(&m);
  }

  lastStatus=*pdwEffect;
  return S_OK;
}
HRESULT __stdcall DragOver(struct IDropTarget *this, DWORD grfKeyState,POINTL pt,DWORD *pdwEffect) {
  POINT ptx;
  cmdStruct cmd;
  *pdwEffect=lastStatus; // DROPEFFECT_COPY
  if (!dragging) {
    ptx.x=pt.x;
    ptx.y=pt.y;
    ScreenToClient(mainWindow,&ptx);
    cmd.cmd=CMD_MOUSEMOVE;
    cmd.param1=ptx.x;
    cmd.param2=ptx.y;
    transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);
  }
  return S_OK;
}
HRESULT __stdcall DragLeave(struct IDropTarget *this) {
  cmdStruct cmd;

  if (serversocket!=SOCK_DISCONNECTED || clientsocket==SOCK_DISCONNECTED || lastStatus==DROPEFFECT_NONE)
    return DROPEFFECT_NONE;

  cmd.cmd=CMD_DRAGDROP;
  cmd.param1=CMD_DRAGDROP_STOP;
  transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);
  EnterCriticalSection(&flistlock);
  deleteFileList(&file_list);
  LeaveCriticalSection(&flistlock);
  return S_OK;
}

int recurseFile(char *tbuf, int rpos) {
  datalist *dlist;
  HANDLE file;
  WIN32_FILE_ATTRIBUTE_DATA attr;
  WIN32_FIND_DATA fd;
  HANDLE fdh;
  char *tname,*xname;
  int ctr=0;

  if (!rpos) 
    rpos=(int)(strrchr(tbuf,'\\')+1-tbuf);

  if (!GetFileAttributesEx(tbuf,GetFileExInfoStandard, &attr)) return;

  if (!(attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    if ((file=CreateFile(tbuf,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL))==INVALID_HANDLE_VALUE) {
      MAlert("Couldnt get access to: %s", tbuf+rpos);
      return;
    }

  if (!GetFileAttributesEx(tbuf,GetFileExInfoStandard, &attr)) return; // in case the file changed after the first call to this function and CreateFile.

  dlist=(datalist *)malloc(sizeof(datalist)+sizeof(fileInfo));
  if (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) dlist->size=0;
  else dlist->size=(attr.nFileSizeHigh*(MAXDWORD+1))+attr.nFileSizeLow;
  dlist->offset=0;
  dlist->type=DATATYPE_FILE;
  dlist->next=NULL;
  ((fileInfo*)dlist_dataptr(dlist))->name=strdup(tbuf+rpos);
  ((fileInfo*)dlist_dataptr(dlist))->file=file;
  ((fileInfo*)dlist_dataptr(dlist))->event1=NULL;
  ((fileInfo*)dlist_dataptr(dlist))->event2=NULL;
  ((fileInfo*)dlist_dataptr(dlist))->attr=attr;
  ctr+=sizeof(int)+strlen(tbuf+rpos)+sizeof(attr);
  file_counter++;

  if (!file_list) {
    file_list=dlist;
    file_end=dlist;
  }
  else {
    file_end->next=dlist;
    file_end=dlist;
  }

  if (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    tname=malloc(strlen(tbuf)+3);
    sprintf(tname,"%s\\*",tbuf);
    if ((fdh=FindFirstFile(tname,&fd))!=INVALID_HANDLE_VALUE) {
      if (strcmp(fd.cFileName,".") && strcmp(fd.cFileName,"..")) {
        xname=malloc(strlen(tbuf)+2+strlen(fd.cFileName));
        sprintf(xname,"%s\\%s",tbuf,fd.cFileName);
        ctr+=recurseFile(xname,rpos);
        free(xname);
      }
      while (FindNextFile(fdh,&fd)) {
        if (!strcmp(fd.cFileName,".") || !strcmp(fd.cFileName,"..")) continue;
        xname=malloc(strlen(tbuf)+2+strlen(fd.cFileName));
        sprintf(xname,"%s\\%s",tbuf,fd.cFileName);
        ctr+=recurseFile(xname,rpos);
        free(xname);
      }
      FindClose(fdh);
    }
    free(tname);
  }
  return ctr;
}

//#define dbgAction Alert("x %i", __LINE__);
//#define dbgAction printf("%s %i %s\n", __FUNCTION__, __LINE__,*(char**)(((BYTE*)this)-sizeof(int)-sizeof(LPIID)-sizeof(char *)));
//#define dbgActionT printf("%s %i\n", __FUNCTION__, __LINE__);
#define dbgAction ;
#define dbgActionT ;

#define StreamFn(Fn) (*((IStream **)(((BYTE*)this)+sizeof(IStream))))->lpVtbl->Fn(*((IStream **)(((BYTE*)this)+sizeof(IStream)))

ULONG __stdcall AddRef(IUnknown *this) {
//printf("%i ",1+(*(int *)(((BYTE*)this)-sizeof(int))));
dbgAction
  return ++(*(int *)(((BYTE*)this)-sizeof(int)));
}

ULONG __stdcall Release(IUnknown *this) {
//printf("%i ",(*(int *)(((BYTE*)this)-sizeof(int)))-1);
dbgAction
  int i=--(*(int *)(((BYTE*)this)-sizeof(int)));

  if (i==0) {
    if (memcmp(&IID_IStream,*(LPIID*)(((BYTE*)this)-sizeof(int)-sizeof(LPIID)),sizeof(IID))==0) {
      StreamFn(Release));
    }
    free(this->lpVtbl);
    free(((BYTE*)this)-sizeof(int)-sizeof(LPIID)-sizeof(char *));
  }
  return i;
}

HRESULT __stdcall QueryInterface(IUnknown *this, REFIID riid, void **ppvObject) {
dbgAction
  if (!ppvObject)
    return E_INVALIDARG;
  *ppvObject = NULL;
  if (memcmp((LPIID)riid, &IID_IUnknown, sizeof(IID))==0 || memcmp((LPIID)riid,*(LPIID*)(((BYTE*)this)-sizeof(int)-sizeof(LPIID)),sizeof(IID))==0) {
    this->lpVtbl->AddRef(this);
    *ppvObject=this;
    return S_OK;
  }
  
  return E_NOINTERFACE;
}

#define createObject(a,b,c,d) createObjectX(a,b,c,d,#c)

void *createObjectX(int size1, int size2, REFIID riid, int extradatasize, char *name) {
  IUnknown *dObj;
dbgActionT
//printf("%s\r\n",name);
  if (size1!=sizeof(void *)) {
    Alert("Mismatched object size: %i", size1);
    ExitProcess(0);
  }
  dObj=malloc(size1+extradatasize+sizeof(int)+sizeof(LPIID)+sizeof(char *))+sizeof(int)+sizeof(LPIID)+sizeof(char *);
  dObj->lpVtbl=malloc(size2);
  dObj->lpVtbl->AddRef=AddRef;
  dObj->lpVtbl->Release=Release;
  dObj->lpVtbl->QueryInterface=QueryInterface;
  *(int *)(((BYTE*)dObj)-sizeof(int))=0;

  *(char**)(((BYTE*)dObj)-sizeof(int)-sizeof(LPIID)-sizeof(char *))=strdup(name);

  AddRef(dObj);

  *(LPIID *)(((BYTE*)dObj)-sizeof(int)-sizeof(LPIID))=(LPIID)riid;

  return dObj;
}


HRESULT __stdcall Drop(struct IDropTarget *this, struct IDataObject *pDataObj,DWORD grfKeyState,POINTL pt,DWORD *pdwEffect) {
  IEnumFORMATETC *EnumTypes;
  FORMATETC o;
  cmdStruct cmd;

  *pdwEffect=DROPEFFECT_NONE;
  pDataObj->lpVtbl->EnumFormatEtc(pDataObj,DATADIR_GET, &EnumTypes);
  while (EnumTypes->lpVtbl->Next(EnumTypes,1,&o,NULL)==S_OK) {
    if (o.cfFormat==CF_IDLIST) {
      *pdwEffect=DROPEFFECT_COPY;
      break;
    }
  }
  EnumTypes->lpVtbl->Release(EnumTypes);

  if (*pdwEffect==DROPEFFECT_COPY) {
    cmd.cmd=CMD_DRAGDROP;
    cmd.param1=CMD_DRAGDROP_DROP;
    transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);
  }

  return S_OK;
}

HRESULT __stdcall GiveFeedback(struct IDropSource *this, DWORD dwEffect) {
  cmdStruct cmd;

dbgAction

  if (serversocket==SOCK_DISCONNECTED)
    return DRAGDROP_S_USEDEFAULTCURSORS;

  cmd.cmd=CMD_DRAGDROP;
  cmd.param1=CMD_DRAGDROP_GIVEFEEDBACK;
  cmd.param2=dwEffect;
  transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);

  return DRAGDROP_S_USEDEFAULTCURSORS;
}
HRESULT __stdcall QueryContinueDrag(struct IDropSource *this,BOOL fEscapePressed,DWORD grfKeyState) {
  cmdStruct cmd;

dbgAction

  if (serversocket==SOCK_DISCONNECTED || dragSourceAction==DRAGSOURCE_CANCEL) {
    EnterCriticalSection(&flistlock);
    deleteFileList(&file_list);
    LeaveCriticalSection(&flistlock);
    return DRAGDROP_S_CANCEL;
  }

  if (dragSourceAction==DRAGSOURCE_DROP) {
    return DRAGDROP_S_DROP;
  }
  return S_OK;
}

IDropSource *createDropSource() {
  IDropSource *dropSource;
dbgActionT
  dropSource=createObject(sizeof(IDropSource), sizeof(IDropSourceVtbl), &IID_IDropSource, 0);
  dropSource->lpVtbl->GiveFeedback=GiveFeedback;
  dropSource->lpVtbl->QueryContinueDrag=QueryContinueDrag;
  return dropSource;
}

IDropTarget *createDropTarget() {
  IDropTarget *dropTarget;
dbgActionT
  dropTarget=createObject(sizeof(IDropTarget), sizeof(IDropTargetVtbl), &IID_IDropTarget, 0);
  dropTarget->lpVtbl->DragEnter=DragEnter;
  dropTarget->lpVtbl->DragLeave=DragLeave;
  dropTarget->lpVtbl->DragOver=DragOver;
  dropTarget->lpVtbl->Drop=Drop;
  return dropTarget;
}

HRESULT __stdcall DAdvise(struct IDataObject *this,FORMATETC *pformatetc,DWORD advf,IAdviseSink *pAdvSink,DWORD *pdwConnection) {
dbgAction
  return OLE_E_ADVISENOTSUPPORTED;
}
HRESULT __stdcall DUnadvise(struct IDataObject *this,DWORD dwConnection) {
dbgAction
  return OLE_E_ADVISENOTSUPPORTED;
}
HRESULT __stdcall EnumDAdvise(struct IDataObject *this,IEnumSTATDATA **ppenumAdvise) {
dbgAction
  return OLE_E_ADVISENOTSUPPORTED;
}
HRESULT __stdcall GetCanonicalFormatEtc (struct IDataObject *this,FORMATETC *pFormatEct, FORMATETC *pFormatEtcOut) {
dbgAction
  return E_NOTIMPL;
}
HRESULT __stdcall SetData(struct IDataObject *this,FORMATETC *pFormatEtc, STGMEDIUM *pMedium,  BOOL fRelease) {
dbgAction
  return E_NOTIMPL;
}

#define caseX(x) case x: printf(#x); break;
HRESULT __stdcall StreamRead(IStream *this,void *out,ULONG cb,ULONG *pcbRead) {
  HRESULT i;
  cmdStruct cmd;
  BOOL waited=FALSE;
  datalist *dlist=*((datalist **)objOffset(this,sizeof(IStream *)));
  HANDLE ev[2]={socketEvent,((fileInfo*)dlist_dataptr(dlist))->event1};
dbgAction

  if (dlist->offset==0 && dlist->size>0) {
    cmd.cmd=CMD_DRAGDROP;
    cmd.param1=CMD_DRAGDROP_GET;
    cmd.param2=dlist->index;
    transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);
    SetEvent(((fileInfo*)dlist_dataptr(dlist))->event2);
    waited=TRUE; // if we don't read anything subsequently, then socket closed 
printf("read wait 1 %i", dlist->index);
    WaitForMultipleObjects(2,ev,FALSE,INFINITE);
  }

  *pcbRead=0;
  while ((dlist->offset<dlist->size) && (*pcbRead==0)) {
    i=StreamFn(Read),out,cb,pcbRead);
/*
    switch (i) {
      caseX(S_OK);
      caseX(S_FALSE);
      caseX(E_PENDING);
      caseX(STG_E_ACCESSDENIED);
      caseX(STG_E_INVALIDPOINTER);
      caseX(STG_E_REVERTED);
    }
*/
    dlist->offset+=*pcbRead;

    if (*pcbRead==0) {
      if (waited) {
        break; // Waited and there wasn't anything subsequently in the stream. Socket must have closed.
      }
      SetEvent(((fileInfo*)dlist_dataptr(dlist))->event2);
      waited=TRUE; // if we don't read anything subsequently, then socket closed
printf("read wait 1 %i", dlist->index);
      WaitForMultipleObjects(2,ev,FALSE,INFINITE);
    }
  }
  return S_OK;
}

HRESULT __stdcall StreamWrite(IStream *this,void const *a,ULONG b,ULONG *c) { return StreamFn(Write),a,b,c); };
HRESULT __stdcall StreamSeek(IStream *this,LARGE_INTEGER a, DWORD b, ULARGE_INTEGER*c) { return StreamFn(Seek),a,b,c); };
HRESULT __stdcall StreamSetSize(IStream *this,ULARGE_INTEGER a) { return StreamFn(SetSize),a); };
HRESULT __stdcall StreamCopyTo(IStream *this,IStream *a, ULARGE_INTEGER b,ULARGE_INTEGER *c, ULARGE_INTEGER *d) { return StreamFn(CopyTo),a,b,c,d); };
HRESULT __stdcall StreamCommit(IStream *this,DWORD a) { return StreamFn(Commit),a); };
HRESULT __stdcall StreamRevert(IStream *this) { return StreamFn(Revert)); };
HRESULT __stdcall StreamLockRegion(IStream *this, ULARGE_INTEGER a, ULARGE_INTEGER b, DWORD c) { return StreamFn(LockRegion),a,b,c); };
HRESULT __stdcall StreamUnlockRegion(IStream *this, ULARGE_INTEGER a, ULARGE_INTEGER b, DWORD c) { return StreamFn(UnlockRegion),a,b,c); };
HRESULT __stdcall StreamStat(IStream *this, STATSTG *a, DWORD b) { return StreamFn(Stat),a,b); };
HRESULT __stdcall StreamClone(IStream *this, LPSTREAM *a) {
  IStream *Stream,*tStream;
  HRESULT x;
  if ((x=StreamFn(Clone),&tStream))!=S_OK) return x;

dbgAction
  Stream=createObject(sizeof(IStream), sizeof(IStreamVtbl), &IID_IStream, sizeof(IStream *)+sizeof(datalist *));
  *((IStream **)objOffset(Stream,0))=tStream;
  *((datalist **)objOffset(Stream,sizeof(IStream *)))=*((datalist **)objOffset(this,sizeof(IStream *)));

  #define assignFn(Fn) Stream->lpVtbl->Fn=Stream##Fn;
  assignFn(Read);
  assignFn(Write);
  assignFn(Seek);
  assignFn(SetSize);
  assignFn(CopyTo);
  assignFn(Commit);
  assignFn(Revert);
  assignFn(LockRegion);
  assignFn(UnlockRegion);
  assignFn(Stat);
  assignFn(Clone);

  *a=Stream;

  return S_OK;
};

IStream *createStream() {
  IStream *Stream;
dbgActionT
  Stream=createObject(sizeof(IStream), sizeof(IStreamVtbl), &IID_IStream, sizeof(IStream *)+sizeof(datalist *));
  if (CreateStreamOnHGlobal(NULL, TRUE, (IStream **)(((BYTE*)Stream)+sizeof(IStream)))!=S_OK) Alert("CreateStream failed.");

  #define assignFn(Fn) Stream->lpVtbl->Fn=Stream##Fn;
  assignFn(Read);
  assignFn(Write);
  assignFn(Seek);
  assignFn(SetSize);
  assignFn(CopyTo);
  assignFn(Commit);
  assignFn(Revert);
  assignFn(LockRegion);
  assignFn(UnlockRegion);
  assignFn(Stat);
  assignFn(Clone);

  return Stream;
}


#define FD_PROGRESSUI 0x00004000
HRESULT __stdcall GetDataIn(struct IDataObject *this,FORMATETC *f,STGMEDIUM *m, BOOL allocated) {
  int i=0,j;
  ULONG k;
  IStream *stream;
  FILEGROUPDESCRIPTOR *fd;
  datalist *dlist;
  char buf[4];

  char tbuf[50];
  GetClipboardFormatName(f->cfFormat, tbuf, 50);
  if (!strncmp(tbuf,"File",4))
    printf("%s\r\n",tbuf);

  if (f->dwAspect!=DVASPECT_CONTENT) return DV_E_DVASPECT;

  if (f->cfFormat==CF_FILEDESCRIPTOR) {
    if (!(f->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
    EnterCriticalSection(&flistlock);
    if (f->lindex==-1) {
      m->tymed=TYMED_HGLOBAL;
      m->pUnkForRelease=NULL;
      if (!allocated)
        m->hGlobal=GlobalAlloc(GMEM_MOVEABLE,sizeof(FILEGROUPDESCRIPTOR)+(file_counter-1)*sizeof(FILEDESCRIPTOR));
      if (!m->hGlobal) {
        LeaveCriticalSection(&flistlock);
        return STG_E_MEDIUMFULL;
      }
      fd=GlobalLock(m->hGlobal);
      fd->cItems=file_counter;
      i=0;
      dlist=file_list;
      while (dlist) {
        fd->fgd[i].dwFlags=FD_ATTRIBUTES | FD_CREATETIME | FD_ACCESSTIME | FD_WRITESTIME | FD_FILESIZE | FD_PROGRESSUI;
        fd->fgd[i].dwFileAttributes=((fileInfo*)dlist_dataptr(dlist))->attr.dwFileAttributes;
        fd->fgd[i].ftCreationTime=((fileInfo*)dlist_dataptr(dlist))->attr.ftCreationTime;
        fd->fgd[i].ftLastAccessTime=((fileInfo*)dlist_dataptr(dlist))->attr.ftLastAccessTime;
        fd->fgd[i].ftLastWriteTime=((fileInfo*)dlist_dataptr(dlist))->attr.ftLastWriteTime;
        fd->fgd[i].nFileSizeHigh=((fileInfo*)dlist_dataptr(dlist))->attr.nFileSizeHigh;
        fd->fgd[i].nFileSizeLow=((fileInfo*)dlist_dataptr(dlist))->attr.nFileSizeLow;
        j=strlen(((fileInfo*)dlist_dataptr(dlist))->name)+1;
        memcpy(fd->fgd[i].cFileName,((fileInfo*)dlist_dataptr(dlist))->name,LIMIT(j,0,MAX_PATH));
        fd->fgd[i].cFileName[MAX_PATH-1]=0;
        dlist=dlist->next;
        i++;
      }
      GlobalUnlock(m->hGlobal);
    }
    else {
      dlist=file_list;
      for (i=0; i!=f->lindex && dlist; i++)
        dlist=dlist->next;
      if (!dlist)
        return DV_E_LINDEX;
      m->tymed=TYMED_HGLOBAL;
      m->pUnkForRelease=NULL;
      if (!allocated)
        m->hGlobal=GlobalAlloc(GMEM_MOVEABLE,sizeof(FILEGROUPDESCRIPTOR));
      if (!m->hGlobal) {
        LeaveCriticalSection(&flistlock);
        return STG_E_MEDIUMFULL;
      }
      fd=GlobalLock(m->hGlobal);
      fd->cItems=1;
      fd->fgd[0].dwFlags=FD_ATTRIBUTES | FD_CREATETIME | FD_ACCESSTIME | FD_WRITESTIME | FD_FILESIZE | FD_PROGRESSUI;
      fd->fgd[0].dwFileAttributes=((fileInfo*)dlist_dataptr(dlist))->attr.dwFileAttributes;
      fd->fgd[0].ftCreationTime=((fileInfo*)dlist_dataptr(dlist))->attr.ftCreationTime;
      fd->fgd[0].ftLastAccessTime=((fileInfo*)dlist_dataptr(dlist))->attr.ftLastAccessTime;
      fd->fgd[0].ftLastWriteTime=((fileInfo*)dlist_dataptr(dlist))->attr.ftLastWriteTime;
      fd->fgd[0].nFileSizeHigh=((fileInfo*)dlist_dataptr(dlist))->attr.nFileSizeHigh;
      fd->fgd[0].nFileSizeLow=((fileInfo*)dlist_dataptr(dlist))->attr.nFileSizeLow;
      j=strlen(((fileInfo*)dlist_dataptr(dlist))->name)+1;
      memcpy(fd->fgd[0].cFileName,((fileInfo*)dlist_dataptr(dlist))->name,LIMIT(j,0,MAX_PATH));
      fd->fgd[0].cFileName[MAX_PATH-1]=0;
      GlobalUnlock(m->hGlobal);
    }
    LeaveCriticalSection(&flistlock);
  } else if (f->cfFormat==CF_FILECONTENTS) {
    if (f->lindex==-1) {
      Alert("asked for wrong lindex");
      return DV_E_LINDEX;
    }
    if (!(f->tymed & TYMED_ISTREAM)) return DV_E_TYMED;
    EnterCriticalSection(&flistlock);
    dlist=file_list;
    for (i=0; i!=f->lindex && dlist; i++)
      dlist=dlist->next;
    if (!dlist) { 
      LeaveCriticalSection(&flistlock);
      return DV_E_LINDEX;
    }
    m->tymed=TYMED_ISTREAM;
    m->pUnkForRelease=NULL;
    if (!allocated) {
      if (!(m->pstm=createStream())) { Alert("stream error"); }
    }
    else
      Alert("Unsupported allocated operation.");
    *((datalist **)objOffset(m->pstm,sizeof(IStream *)))=dlist;
    ((fileInfo*)dlist_dataptr(dlist))->stream=m->pstm;
    LeaveCriticalSection(&flistlock);
  } else {
    return DV_E_FORMATETC;
  }
  return S_OK;
}
HRESULT __stdcall GetData(struct IDataObject *this,FORMATETC *f,STGMEDIUM *m) {
dbgAction
  return GetDataIn(this,f,m,FALSE);
}
HRESULT __stdcall GetDataHere(struct IDataObject *this,FORMATETC *f,STGMEDIUM *m) {
dbgAction
  return E_NOTIMPL;
  return GetDataIn(this,f,m,TRUE);
}

HRESULT __stdcall QueryGetData(struct IDataObject *this,FORMATETC *f) {
  int i;
  datalist *dlist;
dbgAction
  if (f->dwAspect!=DVASPECT_CONTENT) return DV_E_DVASPECT;
  if (f->cfFormat==CF_FILEDESCRIPTOR) {
    if (!(f->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
    if (f->lindex!=-1) {
      dlist=file_list;
      for (i=0; i!=f->lindex && dlist; i++)
        dlist=dlist->next;
      if (!dlist)
        return DV_E_LINDEX;
    }
  } else if (f->cfFormat==CF_FILECONTENTS) {
    if (!(f->tymed & TYMED_ISTREAM)) return DV_E_TYMED;
    if (f->lindex!=-1) {
      dlist=file_list;
      for (i=0; i!=f->lindex && dlist; i++)
        dlist=dlist->next;
      if (!dlist)
        return DV_E_LINDEX;
    } else if (f->lindex==-1)
      return DV_E_LINDEX;
  } else
    return DV_E_FORMATETC;
  return S_OK;
}

#define IEFPosPtr(obj) (int*)(((BYTE*)obj)+sizeof(IEnumFORMATETC))

IEnumFORMATETC *CreateNewIEF(void);

HRESULT __stdcall IEFClone(struct IEnumFORMATETC *this,IEnumFORMATETC **ppenum) {
  IEnumFORMATETC *enumFormat;
dbgAction
  enumFormat=CreateNewIEF();
  *IEFPosPtr(enumFormat)=*IEFPosPtr(this);
  *ppenum=enumFormat;
  return S_OK;
}

HRESULT __stdcall IEFNext(struct IEnumFORMATETC *this,ULONG celt,FORMATETC *rgelt,ULONG *pceltFetched) {
  FORMATETC f;
  int i=0;
dbgAction
  if (*IEFPosPtr(this)>1) return S_FALSE;
  if (pceltFetched)
    *pceltFetched=0;
  while (i<celt && *IEFPosPtr(this)<2) {
    f.ptd=NULL;
    f.dwAspect=DVASPECT_CONTENT;
    if (*IEFPosPtr(this)==0) {
      f.cfFormat=CF_FILEDESCRIPTOR;
      f.tymed=TYMED_HGLOBAL;
      f.lindex=-1;
/*
m.hGlobal=GlobalAlloc(GMEM_MOVEABLE,sizeof(FILEGROUPDESCRIPTOR)+(filenum-1)*sizeof(FILEDESCRIPTOR));
f.lindex is the file descriptor zero based index (during retreival operations)
*/
    }
    else {
      f.cfFormat=CF_FILECONTENTS;
      f.tymed=TYMED_ISTREAM;
      f.lindex=-1;
/*
m.pstm=IStream *;
f.lindex is the file descriptor zero based index (during retreival operations)
*/
    }
    *(rgelt+i)=f;
    if (pceltFetched)
      ++(*pceltFetched);
    ++(*IEFPosPtr(this));
    ++i;
  }
  return S_OK;
}

HRESULT __stdcall IEFReset(struct IEnumFORMATETC *this) {
dbgAction
  *IEFPosPtr(this)=0;
  return S_OK;
}

HRESULT __stdcall IEFSkip(struct IEnumFORMATETC *this, ULONG celt) {
dbgAction
  if (*IEFPosPtr(this)+celt < 3) {
    *IEFPosPtr(this)+=celt;
    return S_OK;
  }
  return S_FALSE;
}

HRESULT __stdcall EnumFormatEtc(struct IDataObject *this,DWORD dwDirection,IEnumFORMATETC **ppenumFormatEtc) {
  dbgAction
  FORMATETC f[2];
  if (dwDirection==DATADIR_GET) {
    f[0].ptd=NULL;
    f[0].dwAspect=DVASPECT_CONTENT;
    f[0].cfFormat=CF_FILEDESCRIPTOR;
    f[0].tymed=TYMED_HGLOBAL;
    f[0].lindex=-1;

    f[1].ptd=NULL;
    f[1].dwAspect=DVASPECT_CONTENT;
    f[1].cfFormat=CF_FILECONTENTS;
    f[1].tymed=TYMED_ISTREAM;
    f[1].lindex=-1;

    return CreateFormatEnumerator(2,f,ppenumFormatEtc);
  }
  else if (dwDirection==DATADIR_SET) {
    return E_NOTIMPL;
  }
  return E_INVALIDARG;
}



HRESULT __stdcall OldEnumFormatEtc(struct IDataObject *this,DWORD dwDirection,IEnumFORMATETC **ppenumFormatEtc) {
  dbgAction
  if (dwDirection==DATADIR_SET)
    return E_NOTIMPL;
  *ppenumFormatEtc=CreateNewIEF();
}

IDataObject *createDataObject() {
  IDataObject *dropObj;

  dropObj=createObject(sizeof(IDataObject), sizeof(IDataObjectVtbl), &IID_IDataObject, 0);
  dropObj->lpVtbl->DAdvise=DAdvise;
  dropObj->lpVtbl->DUnadvise=DUnadvise;
  dropObj->lpVtbl->EnumDAdvise=EnumDAdvise;
  dropObj->lpVtbl->EnumFormatEtc=EnumFormatEtc;
  dropObj->lpVtbl->GetCanonicalFormatEtc=GetCanonicalFormatEtc;
  dropObj->lpVtbl->SetData=SetData;
  dropObj->lpVtbl->GetData=GetData;
  dropObj->lpVtbl->GetDataHere=GetDataHere;
  dropObj->lpVtbl->QueryGetData=QueryGetData;
  return dropObj;
}

IEnumFORMATETC *CreateNewIEF(void) {
  IEnumFORMATETC *enumFormat;
  enumFormat=createObject(sizeof(IEnumFORMATETC), sizeof(IEnumFORMATETCVtbl), &IID_IEnumFORMATETC, sizeof(int));
  enumFormat->lpVtbl->Clone=IEFClone;
  enumFormat->lpVtbl->Next=IEFNext;
  *IEFPosPtr(enumFormat)=0;
  return enumFormat;
}

void createMainWindow (HINSTANCE hInstance) { DBGINT;
//  PaDeviceInfo *devinfo;
  HMIXER mixer;
  MIXERCAPS caps;
  MIXERLINE mixerline,mixerline2;
  MIXERLINECONTROLS mixerlinecontrols;
  MIXERCONTROL *mixercontrol;
  MIXERCONTROLDETAILS mdetails;
  MIXERCONTROLDETAILS_BOOLEAN *bValue;
  INITCOMMONCONTROLSEX commctlinfo;
  RECT dimensions;
  int i,j,k,l,x,numc,selectedid,muxcontrolid;
  HANDLE stdin_rd;
  DWORD dwRead;

  shell32 = LoadLibrary ("shell32.dll");
  wc.cbSize = sizeof(WNDCLASSEX);
  wc.style = CS_GLOBALCLASS | CS_BYTEALIGNCLIENT;
  wc.lpfnWndProc = wndMsgProcessor;
  wc.cbClsExtra = 0;
  wc.cbWndExtra = 4;
  wc.hInstance = hInstance;
  wc.hIcon = LoadIcon(shell32, MAKEINTRESOURCE(309));
  defaultCursor=LoadCursor(NULL, IDC_ARROW);
  wc.hCursor = defaultCursor;
  wc.hbrBackground = NULL;
  wc.lpszMenuName = NULL;
  wc.lpszClassName = classname;
  wc.hIconSm=wc.hIcon;

  commctlinfo.dwSize=sizeof(commctlinfo);
  commctlinfo.dwICC=ICC_BAR_CLASSES | ICC_INTERNET_CLASSES;
  InitCommonControlsEx(&commctlinfo);

  atomClassName = RegisterClassEx(&wc);
  if (atomClassName == INVALID_ATOM)
    return;

  dimensions.left=0;
  dimensions.right=430;
  dimensions.top=0;
  dimensions.bottom=250;

  AdjustWindowRect(&dimensions, MAINWINDOWSTYLE, 0);

//  mainWindow = CreateWindow(classname, "Streamer", MAINWINDOWSTYLE, CW_USEDEFAULT, CW_USEDEFAULT, dimensions.right-dimensions.left, dimensions.bottom-dimensions.top, NULL, NULL, hInstance, NULL);

  mainWindow = CreateDialog(hInstance,MAKEINTRESOURCE(IDD_MAIN),NULL,(DLGPROC)wndMsgProcessor);
  GetWindowRect(mainWindow,&mwRect);
  SendMessage(mainWindow, WM_SETICON, ICON_SMALL, (LPARAM)wc.hIcon);
  RedrawWindow(mainWindow, NULL, NULL, RDW_FRAME | RDW_INVALIDATE);

hwndIP=GetDlgItem(mainWindow,IDC_IP);
hwndConnect=GetDlgItem(mainWindow,IDC_CONNECT);
hwndListen=GetDlgItem(mainWindow,IDC_LISTEN);
hwndText=GetDlgItem(mainWindow,IDC_TEXT);
hwndFPS=GetDlgItem(mainWindow,IDC_FPS);
hwndCRF=GetDlgItem(mainWindow,IDC_CRF);
hwndBITRATE=GetDlgItem(mainWindow,IDC_BITRATE);
hwndABITRATE=GetDlgItem(mainWindow,IDC_ABITRATE);
hwndSCALE=GetDlgItem(mainWindow,IDC_SCALING);
hwndACB=GetDlgItem(mainWindow,IDC_AENABLED);
hwndAudioDevice=GetDlgItem(mainWindow,IDC_SOURCE);
hwndAC=GetDlgItem(mainWindow,IDC_QUALITY);
hwndABL=GetDlgItem(mainWindow,IDC_MINBUFFER);
hwndABM=GetDlgItem(mainWindow,IDC_MAXBUFFER);
hwndSPEED=GetDlgItem(mainWindow,IDC_SPEED);
hwndFD=GetDlgItem(mainWindow,IDC_FASTDECODE);

//        ps.rcPaint.left=ps.rcPaint.top=0;
//        ps.rcPaint.bottom=dHeight;
//        ps.rcPaint.right=dWidth;
//  InvalidateRgn(mainWindow, NULL, FALSE);
//  UpdateWindow(mainWindow);
//  RedrawWindow(mainWindow, NULL, NULL, RDW_INTERNALPAINT);
//        FillRect(mainhDC, &ps.rcPaint, (HBRUSH) (COLOR_WINDOW+1));

  mainhDC=GetDC(mainWindow);

  mFont = CreateFont (-MulDiv(8, GetDeviceCaps(mainhDC, LOGPIXELSY), 72), 0, 0, 0, FW_DONTCARE, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, TEXT("Ms Shell Dlg"));
  SendMessage(mainWindow, WM_SETFONT, (int)mFont, TRUE);

//  hwndIP = CreateWindowEx(0, WC_IPADDRESS, "IP", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 130, 22, mainWindow, (HMENU) NULL, hInstance, NULL);
//DLG  hwndIP = CreateWindowEx(0, WC_EDIT, NULL, ES_AUTOHSCROLL | WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER, 0, 0, 130, 22, mainWindow, (HMENU) NULL, hInstance, NULL);
//DLG  hwndConnect = CreateWindowEx(0, "BUTTON", "Connect", WS_CHILD | WS_VISIBLE | BS_FLAT | WS_TABSTOP, 130,  0, 130, 22, mainWindow, (HMENU) NULL, hInstance, NULL);
//DLG  hwndListen = CreateWindowEx(0, "BUTTON", "Start Server", WS_CHILD | WS_VISIBLE | BS_FLAT | WS_TABSTOP, 300,  0, 130, 22, mainWindow, (HMENU) NULL, hInstance, NULL);
//DLG  hwndText = CreateWindowEx(0, WC_EDIT, NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 0, 22, 430, 100, mainWindow, (HMENU) NULL, hInstance, NULL);

//DLG  hwndCRF  = CreateWindowEx(0, WC_COMBOBOX, NULL, WS_DISABLED | CBS_DROPDOWN | WS_VSCROLL | CBS_HASSTRINGS | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE | BS_FLAT | WS_TABSTOP, 30, 124, 70, 200, mainWindow, (HMENU) NULL, hInstance, NULL);
//DLG  hwndCRFL = CreateWindowEx(0, WC_STATIC, "CRF:", SS_LEFT | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE | BS_FLAT, 0, 127, 30, 22, mainWindow, (HMENU) NULL, hInstance, NULL);
  for (i=1; i<=60; i++) { DBGINT;
    sprintf(volatile_buf,"%i",i);
    SendMessage(hwndCRF,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) volatile_buf); 
  }
  SendMessage(hwndCRF, CB_SETCURSEL, (WPARAM)24, (LPARAM)0);

//DLG  hwndFPS = CreateWindowEx(0, WC_COMBOBOX, NULL, WS_DISABLED | CBS_DROPDOWN | WS_VSCROLL | CBS_HASSTRINGS | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE | BS_FLAT | WS_TABSTOP, 140, 124, 70, 200, mainWindow, (HMENU) NULL, hInstance, NULL);
//DLG  hwndFPSL = CreateWindowEx(0, WC_STATIC, "FPS:", SS_LEFT | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE | BS_FLAT, 110, 127, 30, 22, mainWindow, (HMENU) NULL, hInstance, NULL);
  for (i=1; i<=120; i++) { DBGINT;
    sprintf(volatile_buf,"%i",i);
    SendMessage(hwndFPS,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) volatile_buf); 
  }
  SendMessage(hwndFPS, CB_SETCURSEL, (WPARAM)31, (LPARAM)0);

//DLG  hwndBITRATE  = CreateWindowEx(0, WC_COMBOBOX, NULL, WS_DISABLED | CBS_DROPDOWN | WS_VSCROLL | CBS_HASSTRINGS | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE | BS_FLAT | WS_TABSTOP, 279, 125, 150, 200, mainWindow, (HMENU) NULL, hInstance, NULL);
//DLG  hwndBITRATEL = CreateWindowEx(0, WC_STATIC, "V. Bitrate:", SS_LEFT | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE | BS_FLAT, 212, 128, 67, 22, mainWindow, (HMENU) NULL, hInstance, NULL);
  for (i=100; i<=10000; i+=100) { DBGINT;
    sprintf(volatile_buf,"%i kbytes/s",i);
    SendMessage(hwndBITRATE,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) volatile_buf); 
  }
  SendMessage(hwndBITRATE, CB_SETCURSEL, (WPARAM)9, (LPARAM)0);

//DLG  hwndACB  = CreateWindowEx(0, WC_BUTTON, NULL, BS_AUTOCHECKBOX | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE | BS_FLAT | WS_TABSTOP, 93, 150, 20, 20, mainWindow, (HMENU) NULL, hInstance, NULL);
//DLG  hwndACBL = CreateWindowEx(0, WC_STATIC, "Enable Audio:", SS_LEFT | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE | BS_FLAT, 0, 151, 90, 22, mainWindow, (HMENU) NULL, hInstance, NULL);
  SendMessage(hwndACB,(UINT) BM_SETCHECK,(WPARAM) BST_CHECKED,(LPARAM) 0);

//DLG  hwndSCALE  = CreateWindowEx(0, WC_COMBOBOX, NULL, WS_DISABLED | CBS_DROPDOWNLIST | WS_VSCROLL | CBS_HASSTRINGS | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE | BS_FLAT | WS_TABSTOP, 279, 148, 150, 200, mainWindow, (HMENU) NULL, hInstance, NULL);
//DLG  hwndSCALEL = CreateWindowEx(0, WC_STATIC, "Scaling Method:", SS_LEFT | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE | BS_FLAT, 169, 151, 110, 22, mainWindow, (HMENU) NULL, hInstance, NULL);
  SendMessage(hwndSCALE,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) "Client-Side");
  SendMessage(hwndSCALE,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) "POINT");
  SendMessage(hwndSCALE,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) "FAST BILINEAR");
  SendMessage(hwndSCALE,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) "BILINEAR");
  SendMessage(hwndSCALE,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) "BICUBLIN");
  SendMessage(hwndSCALE,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) "BICUBIC");
  SendMessage(hwndSCALE,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) "SPLINE");
  SendMessage(hwndSCALE,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) "SINC");
  SendMessage(hwndSCALE, CB_SETCURSEL, (WPARAM)2, (LPARAM)0);

  SendMessage(hwndSPEED,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) "Ultrafast");
  SendMessage(hwndSPEED,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) "Superfast");
  SendMessage(hwndSPEED,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) "Veryfast");
  SendMessage(hwndSPEED,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) "Faster");
  SendMessage(hwndSPEED,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) "Fast");
  SendMessage(hwndSPEED,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) "Medium");
  SendMessage(hwndSPEED,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) "Slow");
  SendMessage(hwndSPEED,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) "Slower");
  SendMessage(hwndSPEED,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) "Veryslow");
  SendMessage(hwndSPEED,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) "Placebo");
  SendMessage(hwndSPEED, CB_SETCURSEL, (WPARAM)0, (LPARAM)0);

//DLG  hwndAC  = CreateWindowEx(0, WC_COMBOBOX, NULL, WS_DISABLED | CBS_DROPDOWN | WS_VSCROLL | CBS_HASSTRINGS | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE | BS_FLAT | WS_TABSTOP, 120, 173, 70, 200, mainWindow, (HMENU) NULL, hInstance, NULL);
//DLG  hwndACL = CreateWindowEx(0, WC_STATIC, "Audio Quality:", SS_LEFT | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE | BS_FLAT, 0, 176, 118, 22, mainWindow, (HMENU) NULL, hInstance, NULL);
  for (i=0; i<=10; i++) { DBGINT;
    sprintf(volatile_buf,"%i",i);
    SendMessage(hwndAC,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) volatile_buf); 
  }
  SendMessage(hwndAC, CB_SETCURSEL, (WPARAM)10, (LPARAM)0);

//DLG  hwndABITRATE  = CreateWindowEx(0, WC_COMBOBOX, NULL, WS_DISABLED | CBS_DROPDOWN | WS_VSCROLL | CBS_HASSTRINGS | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE | BS_FLAT | WS_TABSTOP, 279, 173, 150, 200, mainWindow, (HMENU) NULL, hInstance, NULL);
//DLG  hwndABITRATEL = CreateWindowEx(0, WC_STATIC, "A. Bitrate:", SS_LEFT | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE | BS_FLAT, 212, 176, 67, 22, mainWindow, (HMENU) NULL, hInstance, NULL);
  for (i=1; i<=64; i+=1) { DBGINT;
    sprintf(volatile_buf,"%i kbytes/s",i);
    SendMessage(hwndABITRATE,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) volatile_buf); 
  }
  SendMessage(hwndABITRATE, CB_SETCURSEL, (WPARAM)63, (LPARAM)0);

//DLG  hwndABL  = CreateWindowEx(0, WC_COMBOBOX, NULL, WS_DISABLED | CBS_DROPDOWN | WS_VSCROLL | CBS_HASSTRINGS | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE | BS_FLAT | WS_TABSTOP, 279, 198, 70, 200, mainWindow, (HMENU) NULL, hInstance, NULL);
//DLG  hwndABM  = CreateWindowEx(0, WC_COMBOBOX, NULL, WS_DISABLED | CBS_DROPDOWN | WS_VSCROLL | CBS_HASSTRINGS | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE | BS_FLAT | WS_TABSTOP, 359, 198, 70, 200, mainWindow, (HMENU) NULL, hInstance, NULL);
//DLG  hwndABLL1 = CreateWindowEx(0, WC_STATIC, "A. Buffer Target:", SS_LEFT | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE | BS_FLAT, 170, 201, 105, 22, mainWindow, (HMENU) NULL, hInstance, NULL);
//DLG  hwndABLL2 = CreateWindowEx(0, WC_STATIC, "-", SS_LEFT | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE | BS_FLAT, 351, 201, 8, 22, mainWindow, (HMENU) NULL, hInstance, NULL);
  for (i=20; i<=800; i+=10) { DBGINT;
    sprintf(volatile_buf,"%ims",i);
    SendMessage(hwndABL,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) volatile_buf); 
  }
  SendMessage(hwndABL, CB_SETCURSEL, (WPARAM)0, (LPARAM)0);
  for (i=80; i<=860; i+=10) { DBGINT;
    sprintf(volatile_buf,"%ims",i);
    SendMessage(hwndABM,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) volatile_buf); 
  }
  SendMessage(hwndABM, CB_SETCURSEL, (WPARAM)0, (LPARAM)0);

//DLG  hwndAudioDevice  = CreateWindowEx(0, WC_COMBOBOX, NULL, CBS_DROPDOWNLIST | WS_VSCROLL | CBS_HASSTRINGS | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE | BS_FLAT | WS_TABSTOP, 179, 224, 250, 200, mainWindow, (HMENU) NULL, hInstance, NULL);
//DLG  hwndAudioDeviceL = CreateWindowEx(0, WC_STATIC, "Audio Input:", SS_LEFT | WS_CHILD | WS_OVERLAPPED | WS_VISIBLE | BS_FLAT, 99, 227, 80, 22, mainWindow, (HMENU) NULL, hInstance, NULL);

  i=waveInGetNumDevs();
  l=0;

  while (--i >= 0) { DBGINT;
    mixerOpen(&mixer,i,0,0,MIXER_OBJECTF_WAVEIN);
    mixerGetDevCaps((UINT_PTR)mixer,&caps,sizeof(caps));
    for (j=0; j < caps.cDestinations; j++) { DBGINT;
      mixerline.dwDestination = j;
      mixerline.cbStruct = sizeof(MIXERLINE);
      mixerGetLineInfo((HMIXEROBJ)mixer, &mixerline, MIXER_GETLINEINFOF_DESTINATION | MIXER_OBJECTF_HMIXER);
      mixerline2=mixerline;

      mixercontrol=malloc(sizeof(MIXERCONTROL)*mixerline.cControls);
      mixerlinecontrols.cbStruct=sizeof(MIXERLINECONTROLS);
      mixerlinecontrols.dwLineID=mixerline.dwLineID;
      mixerlinecontrols.cControls=mixerline.cControls;
      mixerlinecontrols.cbmxctrl=sizeof(MIXERCONTROL);
      mixerlinecontrols.pamxctrl=mixercontrol;
      mixerGetLineControls((HMIXEROBJ)mixer, &mixerlinecontrols, MIXER_GETLINECONTROLSF_ALL | MIXER_OBJECTF_HMIXER);
      muxcontrolid=MAKEWORD(-100,-100);
      for (x=0; x < mixerline.cControls; x++) { DBGINT;
        if (mixercontrol[x].dwControlType==MIXERCONTROL_CONTROLTYPE_MUX) { DBGINT;
          muxcontrolid=MAKEWORD(mixercontrol[x].dwControlID,mixercontrol[x].cMultipleItems);

          mdetails.cbStruct=sizeof(mdetails);
          mdetails.dwControlID=mixercontrol[x].dwControlID;
          mdetails.cChannels=1;
          mdetails.hwndOwner=NULL;
          mdetails.cMultipleItems=mixercontrol[x].cMultipleItems;
          mdetails.cbDetails=sizeof(MIXERCONTROLDETAILS_BOOLEAN);
          bValue=malloc(mdetails.cbDetails*mdetails.cMultipleItems);
          mdetails.paDetails=bValue;
          mixerGetControlDetails((HMIXEROBJ)mixer, &mdetails, MIXER_GETCONTROLDETAILSF_VALUE | MIXER_OBJECTF_HMIXER);
          if (selectedid!=-2) { DBGINT;
            selectedid=-1;
            for (x=0; x<mdetails.cMultipleItems; x++)
              if (bValue[x].fValue==1) selectedid=mdetails.cMultipleItems-1-x;
          }
          free(bValue);
          break;
        }
      }
      free(mixercontrol);

      numc=mixerline.cConnections;
      for (k=0; k < numc; k++) { DBGINT;
        mixerline.dwDestination = j;
        mixerline.dwSource = k;
        mixerline.cbStruct = sizeof(MIXERLINE);
        mixerGetLineInfo((HMIXEROBJ)mixer, &mixerline, MIXER_GETLINEINFOF_SOURCE | MIXER_OBJECTF_HMIXER);
//Alert("%i%i%i Mixer: %s Dest: %s Source: %s",i,j,k,caps.szPname,mixerline2.szName,mixerline.szName);
//        sprintf(volatile_buf,"%s: %s",mixerline2.szName,mixerline.szName);
        sprintf(volatile_buf,"%s: %s",mixerline.szName,caps.szPname);
        SendMessage(hwndAudioDevice,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) volatile_buf);
        SendMessage(hwndAudioDevice,(UINT) CB_SETITEMDATA,(WPARAM) l++,MAKELONG(MAKEWORD(i,k),muxcontrolid));
        if (strlen(mixerline.szName)>9 && memcmp(mixerline.szName,"Stereo Mix",10)==0) { DBGINT;
          SendMessage(hwndAudioDevice, CB_SETCURSEL, (WPARAM)l-1, (LPARAM)0);
          setMuxInput(i, LOBYTE(muxcontrolid), HIBYTE(muxcontrolid), k);
          selectedid=-2;
        } else if (strlen(caps.szPname)>9 && memcmp(caps.szPname,"Stereo Mix",10)==0) { DBGINT;
          SendMessage(hwndAudioDevice, CB_SETCURSEL, (WPARAM)l-1, (LPARAM)0);
          selectedid=-2;
        }
      }
      if (selectedid>-1)
        SendMessage(hwndAudioDevice, CB_SETCURSEL, (WPARAM)selectedid, (LPARAM)0);
    }
    mixerClose(mixer);
  }

/*
  i=Pa_GetDeviceCount();
  j=0;
  while (--i >= 0) { DBGINT;
    devinfo=(PaDeviceInfo *)Pa_GetDeviceInfo(i);
    if (devinfo->maxInputChannels>0) { DBGINT;
      sprintf(volatile_buf,"%s",devinfo->name);
      SendMessage(hwndAudioDevice,(UINT) CB_ADDSTRING,(WPARAM) 0,(LPARAM) volatile_buf);
      SendMessage(hwndAudioDevice,(UINT) CB_SETITEMDATA,(WPARAM) j++,(LPARAM) i);
    }
  }
*/

//  drawWindow = CreateWindow(classname, "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER, 0, 0, 1000, 1000, mainWindow, NULL, hInstance, NULL);

//  SendMessage(hwndIP, IPM_SETADDRESS, 0, MAKEIPADDRESS(127,0,0,1));
  SendMessage(hwndConnect, WM_SETFONT, (int)mFont, TRUE);

  if (GetCommandLine() && strcmp("RECOVERY",GetCommandLine())==0) { DBGINT;
    ipcReady=654321;
    while (ipcReady!=123456)
      Sleep(1);

    MEMORY_BARRIER; // read-acquire

    SetWindowText(hwndIP, ipcMessage+1);

    if (ipcMessage[0]!=0)
      SendMessage(mainWindow, WM_COMMAND, (WPARAM)MAKELONG(GetDlgCtrlID(hwndListen),BN_CLICKED), (LPARAM)hwndListen);

    MEMORY_BARRIER; // write-release ... not really necessary because we're not modifying any variable other than ipcready, so doesn't matter what order it is written from the cache in

    ipcReady=654321;
    while (ipcReady!=123456)
      Sleep(1);

    MEMORY_BARRIER; // read-acquire

    MAlert("Application crashed (%s), restarted ...",ipcMessage);
/*
    stdin_rd=GetStdHandle(STD_INPUT_HANDLE);
    if (stdin_rd != INVALID_HANDLE_VALUE) { DBGINT;
      ReadFile(stdin_rd, &i, sizeof(i), &dwRead, NULL);
      ReadFile(stdin_rd, passphrase, i, &dwRead, NULL);
      ReadFile(stdin_rd, &i, sizeof(i), &dwRead, NULL);
      ReadFile(stdin_rd, tbuf, i, &dwRead, NULL);
      SetWindowText(hwndIP, tbuf);
      SendMessage(mainWindow, WM_COMMAND, (WPARAM)MAKELONG(GetDlgCtrlID(hwndListen),BN_CLICKED), (LPARAM)hwndListen);
      ReadFile(stdin_rd, &i, sizeof(i), &dwRead, NULL);
      ReadFile(stdin_rd, tbuf, i, &dwRead, NULL);
      CloseHandle(stdin_rd);
      MAlert("Application crashed (%s), restarted server ...",tbuf);
    }
*/
  } else {
    SetWindowText(hwndIP, "127.0.0.1:10900");
  }

  CF_FILECONTENTS=RegisterClipboardFormat(CFSTR_FILECONTENTS);
  CF_FILEDESCRIPTOR=RegisterClipboardFormat(CFSTR_FILEDESCRIPTOR);
  CF_IDLIST=RegisterClipboardFormat(CFSTR_SHELLIDLIST);
  CF_HTML=RegisterClipboardFormat("HTML Format");
  CF_RTF=RegisterClipboardFormat("Rich Text Format");
  nextClipViewer=SetClipboardViewer(mainWindow);

  if (!encryption)
    MAlert("Warning: Encryption is disabled.");

  if (OleInitialize(NULL)!=S_OK) ExitProcess(0);

  dropTarget=createDropTarget();

  RegisterDragDrop(mainWindow,dropTarget);
}

#define WAVEOUT_HEADERS 100 // guarantees that we will never be more than 100 audio frames (frameDuration ms per frame, currently 10) behind
#define PLAYBACK_BUFFER_SIZE waveHeaderBufferSize*WAVEOUT_HEADERS+1
cBuffer *playBuffer=NULL;
hdrInfo *wohdrs=NULL;
HANDLE process_audio_thread=NULL,nonblocking_send_thread=NULL;

DWORD WINAPI workerThread(LPVOID in) { DBGINT;
  while (TRUE)
    SleepEx(INFINITE,TRUE);
}

VOID CALLBACK exitWorkerAPC(ULONG_PTR dwParam) { DBGINT;
  MEMORY_BARRIER;
  if (dwParam) *((int*)dwParam)=1;
  ExitThread(1);
}

DWORD WINAPI nbsThread(LPVOID in) { DBGINT;
  datablock d;
  int i,j=-1;
  EnterCriticalSection(&nbslock);

  while (audio_recording_active) {
    d=cb_read_buf(abuffer, 4, FALSE, TRUE, "abuffer"); // blocking call
    if (d.size==0) break; // blocking operation cancelled; means we're closing
    d=cb_read_buf(abuffer, 4+*(int*)d.data, FALSE, TRUE, "abuffer"); // blocking call
    if (d.size==0) break; // blocking operation cancelled; means we're closing
    transmit_bulk(clientsocket, d.data+4, d.size-4, TRANSMIT_AUDIO); 
    cb_read(abuffer, d.size, TRUE);
  }

  LeaveCriticalSection(&nbslock);
  ExitThread(1);
}


const char *ablabel="Encode Audio";
VOID CALLBACK processaudioAPC(ULONG_PTR dwParam) { DBGINT;
  WAVEHDR *apending;
  int i;
  static BOOL warned=FALSE;
  clock_t a;
  datablocks w;
  EnterCriticalSection(&paAPClock);
  MEMORY_BARRIER; // read-acquire

  apending=(WAVEHDR *)dwParam;

  if (waveInUnprepareHeader(waveIn, apending, sizeof(WAVEHDR))!=MMSYSERR_NOERROR) { DBGINT;
    Alert("Could not unprepare waveIn header.");
    SafeShutdown();
  }

  if (!audio_recording_active) { DBGINT;
    free(apending->lpData);
    MEMORY_BARRIER; // write-release
    ((hdrInfo *)dwParam)->state=UNPREPARED;

    LeaveCriticalSection(&paAPClock);
    return; // no need to re-enqueue buffers once recording has stopped
  }

  if (apending->dwBytesRecorded!=waveHeaderBufferSize) { DBGINT;
    Debug("Dropped audio frame because of mismatched buffer sizes %i/%i.",apending->dwBytesRecorded,waveHeaderBufferSize);
  } else {
    benchmark_range(ablabel,BM_START,&a);
    i = opus_encode(aenc, (const opus_int16 *) apending->lpData, waveBufferFrameSize, aencdecbuf, maxAudioOutputBytes);
    benchmark_range(ablabel,BM_STOP,&a);
    if (i<0) { DBGINT;
      Alert("Could not encode Opus audio: %i", i);
      SafeShutdown();
    }
    if (i>AENCDECBUF_SIZE) {
      Alert("AENCDECBUF_SIZE exceeded: %i", i);
      SafeShutdown();
    }

    while (!cb_write((BYTE*)&i,abuffer,sizeof(i))) {
      if (!warned) {
        warned=TRUE;
        MAlert("processAudioAPC overflow!");
      }
      Sleep(1);
    } 
    while (!cb_write(aencdecbuf,abuffer,i)) {
      if (!warned) {
        warned=TRUE;
        MAlert("processAudioAPC overflow!");
      }
      Sleep(1);
    }
  }

  if (waveInPrepareHeader(waveIn, apending, sizeof(WAVEHDR))!=MMSYSERR_NOERROR) { DBGINT;
    Alert("Could not prepare waveIn header.");
    SafeShutdown();
  }

  if (waveInAddBuffer(waveIn, apending, sizeof(WAVEHDR))!=MMSYSERR_NOERROR) { DBGINT;
    Alert("Could not add wave buffer.");
    SafeShutdown();
  }
  LeaveCriticalSection(&paAPClock);
}

void CALLBACK waveInProc(HWAVEIN waveIn, UINT uMsg, DWORD dwInstance, DWORD dwParam1, DWORD dwParam2) { DBGINT;
  if (uMsg!=WIM_DATA) return;
  QueueUserAPC(processaudioAPC, process_audio_thread, (ULONG_PTR)dwParam1);
}

void get_CELT_stream_input(void) { DBGINT;
  WAVEFORMATEX sformat;
  HWAVEOUT pwhix;
  BYTE *waveBuffer;
  static double fAngle;
  int i;

  audio_recording_active=TRUE;
  MEMORY_BARRIER;
  process_audio_thread=CreateThread(NULL, 0, workerThread, (LPVOID)1, 0, NULL);

  abuffer=cb_create(ASEND_BUFFER_SIZE,0);
  nonblocking_send_thread=CreateThread(NULL, 0, nbsThread, 0, 0, NULL);

  sformat.wFormatTag=WAVE_FORMAT_PCM;
  sformat.nChannels=2;
//  sformat.nSamplesPerSec=44100;
  sformat.nSamplesPerSec=48000;
  sformat.wBitsPerSample=16;
  sformat.nBlockAlign=sformat.nChannels*(sformat.wBitsPerSample/8);
  sformat.nAvgBytesPerSec=sformat.nSamplesPerSec*sformat.nBlockAlign;
  sformat.cbSize=0;

  i=SendMessage(hwndAudioDevice,(UINT) CB_GETITEMDATA,(WPARAM) ComboBox_GetCurSel(hwndAudioDevice), 0);

  if (waveInOpen(&waveIn,LOBYTE(LOWORD(i)),&sformat,(UINT)waveInProc,0,CALLBACK_FUNCTION | WAVE_MAPPED)!=MMSYSERR_NOERROR) { DBGINT;
    Alert("Could not open wave device.");
    SafeShutdown();
  }

  for (i=0; i<WAVEINHDRS; i++) { DBGINT;
    ZeroMemory(&whdrs[i],sizeof(hdrInfo));
    whdrs[i].hdr.lpData=(char *)malloc(waveHeaderBufferSize);
    whdrs[i].hdr.dwBufferLength=waveHeaderBufferSize;

    if (waveInPrepareHeader(waveIn, &(whdrs[i].hdr), sizeof(WAVEHDR))!=MMSYSERR_NOERROR) { DBGINT;
      Alert("Could not prepare wave header.");
      SafeShutdown();
    }

    whdrs[i].state=PREPARED;
    MEMORY_BARRIER; // write-release

    if (waveInAddBuffer(waveIn, &(whdrs[i].hdr), sizeof(WAVEHDR))!=MMSYSERR_NOERROR) { DBGINT;
      Alert("Could not add wave buffer.");
      SafeShutdown();
    }


    if (audio_enabled && waveInStart(waveIn)!=MMSYSERR_NOERROR) { DBGINT; 
      Alert("Could not start wave capture.");
      SafeShutdown();
    }
  }

  aenc = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &i); // OPUS_APPLICATION_RESTRICTED_LOWDELAY
  if (i!=OPUS_OK) { DBGINT;
    Alert("Could not start Opus encoder.");
    SafeShutdown();
  }
  opus_encoder_ctl(aenc, OPUS_SET_BITRATE(opus_bitrate));
  opus_encoder_ctl(aenc, OPUS_SET_COMPLEXITY(opus_complexity));
  opus_encoder_ctl(aenc, OPUS_SET_SIGNAL(OPUS_AUTO));
}

WAVEHDR *audioBufferHead=NULL, *audioBufferTail=NULL;

BOOL ABCATCHUP = FALSE;

VOID CALLBACK freeaudioAPC(ULONG_PTR dwParam) { DBGINT;
  static BOOL warned=FALSE;
  int i=0;
  WAVEHDR *apending=(WAVEHDR *)dwParam;

  MEMORY_BARRIER; // read-acquire

  while (!((*(volatile DWORD *)&(apending->dwFlags)) & WHDR_DONE)) { DBGINT;
    if (!warned) { DBGINT;
      MAlert("waveOutProc hasn't relinquished its header prior to calling freeAudioAPC. This should never happen.");
      warned=TRUE;
    }
    Sleep(0);
  }

  if ((terr=waveOutUnprepareHeader(waveOut, apending, sizeof(WAVEHDR)))!=MMSYSERR_NOERROR) { DBGINT;
    Alert("Could not unprepare waveOut header. (%i)", terr);
    SafeShutdown();
  }

  if (ABCATCHUP) { DBGINT;
    EnterCriticalSection(&abuflock);
    msec_enqueued-=1000.0f * apending->dwBufferLength / 4 / 48000;
    LeaveCriticalSection(&abuflock);

    MEMORY_BARRIER; // write-release - probably not necessary given this is immediately preceded by a critical section
    ((hdrInfo *)dwParam)->state=UNPREPARED;
    return;
  }

  cb_read(playBuffer, apending->dwBufferLength, TRUE);
  apending->lpData=NULL;

  EnterCriticalSection(&abuflock);
  if (audioBufferHead==(WAVEHDR *)dwParam) { DBGINT;
    audioBufferHead=NULL;
  }
  if (audioBufferTail==(WAVEHDR *)dwParam) { DBGINT;
    if (audioBufferTail->dwUser)
      audioBufferTail=(WAVEHDR *)audioBufferTail->dwUser;
//BUGFIX OF:   audioBufferTail=(WAVEHDR *)((WAVEHDR *)audioBufferTail->dwUser)->dwUser;
    else
      audioBufferTail=NULL;
  }
  msec_enqueued-=1000.0f * apending->dwBufferLength / 4 / 48000;
  LeaveCriticalSection(&abuflock);

  MEMORY_BARRIER; // write-release - probably not necessary given this is immediately preceded by a critical section
  ((hdrInfo *)dwParam)->state=UNPREPARED;
}

void CALLBACK waveOutProc(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) { DBGINT;
  switch (uMsg) { DBGINT;
    case WOM_DONE:
      QueueUserAPC(freeaudioAPC, process_audio_thread, (ULONG_PTR)dwParam1);
      break;
    case WOM_OPEN:
      break;
    case WOM_CLOSE:
      break;
  }
}

void start_CELT_stream_output(void) { DBGINT;
  WAVEFORMATEX sformat;
  int i;

  playBuffer=cb_create(PLAYBACK_BUFFER_SIZE,0);

  wohdrs=malloc(sizeof(hdrInfo)*WAVEOUT_HEADERS);
  ZeroMemory(wohdrs,sizeof(hdrInfo)*WAVEOUT_HEADERS);

  audioBufferHead=audioBufferTail=NULL;

  sformat.wFormatTag=WAVE_FORMAT_PCM;
  sformat.nChannels=2;
//  sformat.nSamplesPerSec=44100;
  sformat.nSamplesPerSec=48000;
  sformat.wBitsPerSample=16;
  sformat.nBlockAlign=sformat.nChannels*(sformat.wBitsPerSample/8);
  sformat.nAvgBytesPerSec=sformat.nSamplesPerSec*sformat.nBlockAlign;
  sformat.cbSize=0;

  if (waveOutOpen(&waveOut,WAVE_MAPPER,&sformat,(UINT)waveOutProc,0,CALLBACK_FUNCTION)!=MMSYSERR_NOERROR) { DBGINT;
    Alert("Could not open waveOut device.");
    SafeShutdown();
  }

  if (waveOutPause(waveOut)!=MMSYSERR_NOERROR) { DBGINT;
    Alert("Could not pause waveOut device.");
    SafeShutdown();
  }

  audio_playback_paused=TRUE;
  audio_playback_active=TRUE;
  process_audio_thread=CreateThread(NULL, 0, workerThread, 0, 0, NULL);

  adec=opus_decoder_create(48000, 2, &i);
  if (i!=OPUS_OK) { DBGINT;
    Alert("Could not create Opus decoder: %i",i);
    SafeShutdown();
  }
}

/*
  Based on this model;
  Initial buffering takes place up to 30ms, but should not exceed the size of playBuffer since it is a little longer (32.5ms).
  If there is a brief network disruption, buffer will play out up to 30ms; hopefully it is only transient and there will be a compensated increase in throughoutput that will fill the buffer back up instantly without the user having to wait.
  If the buffer reaches 0, next time data is received, another Sleep() call is made that should help build the buffer back up to 30ms (will not be the case if data is coming in in spurts-but that kind of connection is not conducive to the applications purpose anyway)
  If a lot of data builds up during a network interruption, it will overwhelm the buffer when network throughoutput improves. Incoming data will be intermittently dropped when the buffer is full until receive speed normalizes and equals playback speed
*/

void reenqueue_buffers(WAVEHDR *thdr) { DBGINT;
  WAVEHDR *xhdr;
  while (thdr) { DBGINT;
    while (((hdrInfo *)thdr)->state==PREPARED)
      Sleep(0);

    MEMORY_BARRIER; // read-acquire

    EnterCriticalSection(&abuflock);
    msec_enqueued+=1000.0f * thdr->dwBufferLength / 4 / 48000;
    LeaveCriticalSection(&abuflock);

    if ((terr=waveOutPrepareHeader(waveOut, thdr, sizeof(WAVEHDR)))!=MMSYSERR_NOERROR) { DBGINT;
      Alert("Could not prepare waveOut header. (%i)", terr);
      SafeShutdown();
    }

    xhdr=(WAVEHDR *)thdr->dwUser;

    ((hdrInfo *)thdr)->state=PREPARED;
    MEMORY_BARRIER; // write-release
    
    if ((terr=waveOutWrite(waveOut, thdr, sizeof(WAVEHDR)))!=MMSYSERR_NOERROR) { DBGINT;
      Alert("Could not write waveOut header 1. (%i) %i %i", terr, (thdr->dwFlags & WHDR_PREPARED), ((hdrInfo *)thdr)->state);
      SafeShutdown();
    }

    thdr=xhdr;
  }


  ABCATCHUP=FALSE;
}

void freeAudioTail(void) { DBGINT;
  if (!audioBufferTail) return;
  while (((hdrInfo *)audioBufferTail)->state==PREPARED)
    Sleep(0);

  MEMORY_BARRIER; // read-acquire

  cb_read(playBuffer, audioBufferTail->dwBufferLength, TRUE);
  audioBufferTail->lpData=NULL;
  audioBufferTail=(WAVEHDR *)audioBufferTail->dwUser;
}

VOID CALLBACK play_audio_buffer_data(ULONG_PTR blen);
VOID CALLBACK play_audio_buffer(ULONG_PTR blen) { DBGINT;
  clock_t a=clock();
  play_audio_buffer_data(blen);
  EnterCriticalSection(&ptclock);
  totalBytes+=blen;
  totalTime+=clock()-a;
  LeaveCriticalSection(&ptclock);
}
VOID CALLBACK play_audio_buffer_data(ULONG_PTR blen) { DBGINT;
  datablocks written;
  static int count=0;
  int i,len,msec_remaining;
  WAVEHDR *hdr1=NULL,*hdr2=NULL;
  BYTE *buffoffset=NULL;

  len = opus_decode(adec, (const unsigned char*)cb_read(abuffer,(int)blen,FALSE), (int)blen, (opus_int16 *) aencdecbuf, maxWaveBufferFrameSize, 0); // returns number of frames; need to adjust number for channels and bits per channel
  cb_read(abuffer,(int)blen,TRUE);
  if (len < 0) { DBGINT;
    Alert("Opus decode error: %i", len);
    SafeShutdown();
  }

  len*=4;

  for (i=0; i<WAVEOUT_HEADERS; i++) { DBGINT;
    if (wohdrs[i].state==UNPREPARED) { DBGINT;
      MEMORY_BARRIER; // read-acquire
      if (!hdr1) { DBGINT;
        hdr1=&(wohdrs[i].hdr);
      }
      else {
        hdr2=&(wohdrs[i].hdr);
        break;
      }
    }
  }

  if (!hdr2) { DBGINT;
    ABCATCHUP=TRUE;
    waveOutReset(waveOut);
    while (!hdr2) { DBGINT;
      if (!hdr1) hdr1=audioBufferTail;
      else hdr2=audioBufferTail;
      freeAudioTail();
    }
    for (i=0; i<10; i++)
      freeAudioTail();
Debug("droptail hdr");
    reenqueue_buffers(audioBufferTail);
  }

  EnterCriticalSection(&abuflock);
  if ((msec_enqueued+(1000.0f * len / 4 / 48000)) > AUDIO_BUFFER_UPPER_LIMIT) { DBGINT;
    msec_remaining=msec_enqueued;
    LeaveCriticalSection(&abuflock);
    ABCATCHUP=TRUE;
    waveOutReset(waveOut);
    while ((msec_remaining+(1000.0f * len / 4 / 48000)) > AUDIO_BUFFER) { DBGINT; // discard down to AUDIO BUFFER length
      if (!audioBufferTail) { DBGINT; // this segment's time len is greater than the whole audio buffer length; discard it
        reenqueue_buffers(audioBufferTail);
        return;
      }
      msec_remaining-=1000.0f * audioBufferTail->dwBufferLength / 4 / 48000;
      freeAudioTail();
    }

Debug("droptail msec");

    reenqueue_buffers(audioBufferTail);
  }
  else {
    LeaveCriticalSection(&abuflock);
  }
  written=cb_disjointed_write(aencdecbuf,playBuffer,len,4); // blocksize is 4; stereo stream with 2 bytes per sample
  if (written.size1==CB_OVERFLOW) { DBGINT;
    ABCATCHUP=TRUE;
    waveOutReset(waveOut);

    while (written.size1==CB_OVERFLOW) { DBGINT;
      if (!audioBufferTail) { DBGINT; // this segment's byte len is greater than the whole audio buffer length; discard it
        reenqueue_buffers(audioBufferTail);
        return;
      }
      freeAudioTail();
      written=cb_disjointed_write(aencdecbuf,playBuffer,len,4); // blocksize is 4; stereo stream with 2 bytes per sample
    }
Debug("droptail y");
    reenqueue_buffers(audioBufferTail);
  }
  EnterCriticalSection(&abuflock);
  if (msec_enqueued == 0 && !audio_playback_paused) { DBGINT;
    if (waveOutPause(waveOut)!=MMSYSERR_NOERROR) { DBGINT;
      Alert("Could not pause waveOut device.");
      SafeShutdown();
    }
    audio_playback_paused=TRUE;
  }

  msec_enqueued += 1000.0f * len / 4 / 48000;
  LeaveCriticalSection(&abuflock);

  ZeroMemory(hdr1,sizeof(WAVEHDR));
  hdr1->lpData=written.data1;
  hdr1->dwBufferLength=written.size1;
  hdr1->dwUser=0;
  EnterCriticalSection(&abuflock);
    if (audioBufferHead)
      audioBufferHead->dwUser = (DWORD_PTR) hdr1;
    audioBufferHead=hdr1;
    if (!audioBufferTail)
      audioBufferTail=hdr1;
  LeaveCriticalSection(&abuflock);

  if (waveOutPrepareHeader(waveOut, hdr1, sizeof(WAVEHDR))!=MMSYSERR_NOERROR) { DBGINT;
    Alert("Could not prepare waveOut header. %i");
    SafeShutdown();
  }

  ((hdrInfo *)hdr1)->state=PREPARED;
  MEMORY_BARRIER; // write-release

  if ((terr=waveOutWrite(waveOut, hdr1, sizeof(WAVEHDR)))!=MMSYSERR_NOERROR) { DBGINT;
    Alert("Could not write waveOut header 2. (%i)", terr);
    SafeShutdown();
  }

  if (written.data2) { DBGINT;
    ZeroMemory(hdr2,sizeof(WAVEHDR));
    hdr2->lpData=written.data2;
    hdr2->dwBufferLength=written.size2;
    hdr2->dwUser=0;
    EnterCriticalSection(&abuflock);
      if (audioBufferHead)
        audioBufferHead->dwUser = (DWORD_PTR) hdr2;
      audioBufferHead=hdr2;
      if (!audioBufferTail)
        audioBufferTail=hdr2;
    LeaveCriticalSection(&abuflock);

    if (waveOutPrepareHeader(waveOut, hdr2, sizeof(WAVEHDR))!=MMSYSERR_NOERROR) { DBGINT;
      Alert("Could not prepare waveOut header. %i");
      SafeShutdown();
    }

    ((hdrInfo *)hdr2)->state=PREPARED;
    MEMORY_BARRIER; // write-release

    if ((terr=waveOutWrite(waveOut, hdr2, sizeof(WAVEHDR)))!=MMSYSERR_NOERROR) { DBGINT;
      Alert("Could not write waveOut header 3. (%i)", terr);
      SafeShutdown();
    }
  }

  EnterCriticalSection(&abuflock);
  if ((msec_enqueued >= AUDIO_BUFFER) && audio_playback_paused) { DBGINT;
    LeaveCriticalSection(&abuflock);
    if (waveOutRestart(waveOut)!=MMSYSERR_NOERROR) { DBGINT;
      Alert("Could not start waveOut device.");
      SafeShutdown();
    }
    audio_playback_paused=FALSE;
  } else
    LeaveCriticalSection(&abuflock);
}

void destroyMainWindow(void) { DBGINT;
  if (mainWindow!=NULL) { DBGINT;
    ReleaseDC (mainWindow, mainhDC);
    DestroyWindow(mainWindow);
  }
  if (shell32!=NULL) { DBGINT;
    FreeLibrary(shell32);
    UnregisterClass(MAKEINTATOM(atomClassName), NULL);
  }
  mainWindow=(HWND)NULL;
  shell32=NULL;
}

char x264log[10000]="";
static void x264logger( void *p_unused, int i_level, const char *psz_fmt, va_list arg ) { DBGINT;
    char *psz_prefix;
    switch( i_level )
    {
        case X264_LOG_ERROR:
            psz_prefix = "error";
            break;
        case X264_LOG_WARNING:
            psz_prefix = "warning";
            break;
        case X264_LOG_INFO:
            psz_prefix = "info";
            break;
        case X264_LOG_DEBUG:
            psz_prefix = "debug";
            break;
        default:
            psz_prefix = "unknown";
            break;
    }
  if (strlen(x264log)>9000) x264log[0]=0;
  sprintf(x264log+strlen(x264log), "%s: ", psz_prefix);
  vsprintf(x264log+strlen(x264log), psz_fmt, arg);
}

// couldn't get this to work
/*
void nalu_processor( x264_t *h, x264_nal_t *nal, void *opaque ) {
     // 1) Have available an output buffer of at least size nal->i_payload*3/2 + 5 + 64.
     // 2) Call x264_nal_encode( h, dst, nal ), where dst is the output buffer.
  BYTE pBuffer[231900]; // MTU_SIZE*3/2 + 5 + 64
  x264_nal_encode( h, pBuffer, nal );
  if (transmit_bulk(clientsocket,nal->p_payload,nal->i_payload,TRANSMIT_VIDEO) == SOCKET_ERROR) { DBGINT;
    clientActive=FALSE;
  }
}
*/

void get_x264_stream_input (HDC* hDCScreen,HDC* hDCScreenCopyBuffer,HWND* Desktop,HBITMAP* Bitmap, BYTE** desktopBitmapData) { DBGINT;
  HWND desktopWindow;
        HBITMAP desktopBitmap;
        BITMAPINFO   bi;
  x264_param_t param;
  char tbuf[50];

  desktopWindow = GetDesktopWindow ();
  GetWindowRect (desktopWindow, &desktopRect);
  dWidth=desktopRect.right-desktopRect.left;
  dHeight=desktopRect.bottom-desktopRect.top;

  *hDCScreen = GetDC (NULL);
//  *hDCScreen = CreateDC("DISPLAY",NULL,NULL,NULL);
  *hDCScreenCopyBuffer = CreateCompatibleDC (*hDCScreen);

        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);    
        bi.bmiHeader.biWidth = dWidth;    
        bi.bmiHeader.biHeight = dHeight;  
        bi.bmiHeader.biPlanes = 1;    
        bi.bmiHeader.biBitCount = 32;    
        bi.bmiHeader.biCompression = BI_RGB;    
        bi.bmiHeader.biSizeImage = 0;  
        bi.bmiHeader.biXPelsPerMeter = 0;    
        bi.bmiHeader.biYPelsPerMeter = 0;    
        bi.bmiHeader.biClrUsed = 0;    
        bi.bmiHeader.biClrImportant = 0;
        desktopBitmap=CreateDIBSection(*hDCScreenCopyBuffer, &bi, DIB_RGB_COLORS, (void **) desktopBitmapData, NULL, 0);
        oldBitmap=SelectObject (*hDCScreenCopyBuffer, desktopBitmap);
//        SetStretchBltMode (*hDCScreenCopyBuffer, HALFTONE);
//        SetBrushOrgEx (*hDCScreenCopyBuffer, 0, 0, NULL);

        if (capture_mode==MODE_DIRECTX)
          initialize_directx_capture();

//*** LEFT OFF HERE
//  BitBlt (*hDCScreenCopyBuffer, 0, 0, dWidth, dHeight, *hDCScreen, desktopRect.left, desktopRect.top, SRCCOPY);
//  StretchBlt (*hDCScreenCopyBuffer, 0, 0, dWidth/4, dHeight/4, *hDCScreen, desktopRect.left, desktopRect.top, dWidth, dHeight, SRCCOPY);
  *Desktop=desktopWindow;
  *Bitmap=desktopBitmap;

  GetWindowText(hwndSPEED,tbuf,50);

  x264_param_default_preset(&param, tbuf, (fast_decode ? "zerolatency,fastdecode" : "zerolatency"));

//  param.pf_log=x264logger;
  param.i_log_level=X264_LOG_INFO;
  param.i_threads = X264_THREADS_AUTO;
  if (scale) { DBGINT;
    param.i_width = wWidth;
    param.i_height = wHeight;
  } else {
    param.i_width = dWidth;
    param.i_height = dHeight;
  }

  param.i_fps_num = fps_goal;
  param.i_fps_den = 1;
  // Intra refresh:
  param.i_keyint_max = X264_KEYINT_MAX_INFINITE; // do not send keyframes (intra-refresh) unless commanded to by client
//  param.i_keyint_max = fps_goal*10; 
  param.b_intra_refresh = 1; // disabled intra refresh since we are using TCP
  param.b_open_gop=1;
  param.i_slice_max_size=MTU_SIZE;

  //Rate control:

/*
  param.rc.i_rc_method = X264_RC_CQP;
  param.rc.i_qp_constant = 50; // 0 - 195
*/

  param.rc.i_rc_method = X264_RC_CRF;
  param.rc.f_rf_constant = crf_goal;

        if (max_bitrate > 0) { DBGINT;
//    param.rc.f_rf_constant_max = 1000;
    param.rc.i_vbv_max_bitrate=max_bitrate*8;
    param.rc.i_vbv_buffer_size=max_bitrate*8/param.i_fps_num;
    param.rc.f_vbv_buffer_init=1;
        }

/*
  param.rc.i_rc_method = X264_RC_ABR;
  param.rc.i_bitrate=100 * 8; // because it's in kilobits / sec
  param.rc.f_rate_tolerance=0.01;
*/

        param.i_csp=X264_CSP_BGRA;
  //For streaming: param.b_repeat_headers = 1;
  param.b_annexb = 1; // Not sure what this is for.
//  param.nalu_process=nalu_processor; - couldn't get this to work
  x264_param_apply_fastfirstpass(&param);
  x264_param_apply_profile(&param, "high444");
  x264encoder = x264_encoder_open(&param);

  // x264_picture_alloc(&pic_in, X264_CSP_BGRA, wWidth, wHeight);
  //  pic_in.img.i_csp=X264_CSP_BGRA;
  //  pic_in.img.i_plane=1;
  //  pic_in.img.i_stride[0]=dWidth * BITSPERPIXEL / 8;
  // oldImgPtr=pic_in.img.plane[0];
  x264_picture_init(&pic_in);
  x264_picture_init(&pic_out);
/*
  if (scale)
    x264_picture_alloc(&pic_out, X264_CSP_BGRA, wWidth, wHeight);
  else
    x264_picture_alloc(&pic_out, X264_CSP_BGRA, dWidth, dHeight);
*/
  pic_in.img.plane[0]=(uint8_t *)*desktopBitmapData;
  pic_in.img.i_stride[0]=((((dWidth * 32) + 31) & ~31) >> 3);
  pic_in.img.i_csp = X264_CSP_BGRA;
}

DWORD WINAPI scaleloop(LPVOID in) {
  AVPicture srcPic,tconverted;

  while (TRUE) {
    EnterCriticalSection(&scalestartlock);
    LeaveCriticalSection(&scalestartlock);

    if (cCtx[0] == NULL) {
      CloseHandle(scale_threads[(int)in]);
      scale_threads[(int)in]=NULL;
      ExitThread(0);
    }

    srcPic.data[0]=desktopBitmapData;
    srcPic.linesize[0]=((((dWidth * 32) + 31) & ~31) >> 3);
    tconverted=converted;

    srcPic.data[0]+=srcPic.linesize[0]*dHeight*((int)in)/4;
    tconverted.data[0]+=tconverted.linesize[0]*wHeight*((int)in)/4;

    sws_scale(cCtx[(int)in], (const uint8_t * const*)srcPic.data, srcPic.linesize, 0, dHeight/4, tconverted.data, tconverted.linesize);

    EnterCriticalSection(&scalecounterlock);
    scalesComplete++;
    LeaveCriticalSection(&scalecounterlock);

    EnterCriticalSection(&scalestoplock);
    LeaveCriticalSection(&scalestoplock);

    EnterCriticalSection(&scalecounterlock);
    scalesComplete++;
    LeaveCriticalSection(&scalecounterlock);
  }
}

void initScaleCtx(void) { DBGINT;
  int i;

  if (scale) { DBGINT;
    pic_in.img.plane[0]=(uint8_t *)converted.data[0];
    pic_in.img.i_stride[0]=converted.linesize[0];

    for (i=0; i<4; i++) { DBGINT;
// SWS_POINT <  SWS_FAST_BILINEAR < SWS_BILINEAR < SWS_BICUBLIN < SWS_BICUBIC < SWS_SPLINE < SWS_SINC
//        cCtx[i] = sws_getContext(dWidth/2, dHeight/2, PIX_FMT_BGRA, wWidth/2, wHeight/2, PIX_FMT_BGRA, SWS_SPLINE, NULL, NULL, NULL);
      if (cCtx[i]!=NULL)
        sws_freeContext(cCtx[i]);
      cCtx[i] = sws_getContext(dWidth, dHeight/4, PIX_FMT_BGRA, wWidth, wHeight/4, PIX_FMT_BGRA, scale_mode, NULL, NULL, NULL);
      if (cCtx[i]==NULL) { DBGINT;
        MsgBox("Cannot initialize the conversion context");
        SafeShutdown();
      }
    }
  } else {
    pic_in.img.plane[0]=(uint8_t *)desktopBitmapData;
    pic_in.img.i_stride[0]=((((dWidth * 32) + 31) & ~31) >> 3);
  }

}

datablock *screen_capture_frame(void) { DBGINT; // returns next frame
  static CURSORINFO ci;
  x264_nal_t* nals;
  int i_nals, i;
  int frame_size;
  static datablock rframe;
  static AVPicture srcPic;
  IDirect3DSurface9* pRenderTarget=NULL, *pCopySurface=NULL, *pDestTarget=NULL;
  D3DLOCKED_RECT pLockedRect;
  SECURITY_ATTRIBUTES secattrib;
  static HANDLE se=NULL;
  clock_t a;
  const char *alabel="Capture";
  const char *blabel="Scale";
  const char *clabel="Encode";

  benchmark_range(alabel,BM_START,&a);

//  static BYTE *imageData=NULL;

  if (capture_mode==MODE_GDI) { DBGINT;
    BitBlt (hDCScreenCopyBuffer, 0, 0, dWidth, dHeight, hDCScreen, desktopRect.left, desktopRect.top, CAPTUREBLT | SRCCOPY); // 
//    if (ci.cbSize==0) { DBGINT;
//      ci.cbSize=sizeof(ci);
//      GetCursorInfo(&ci);
//    }
//    DrawIconEx(hDCScreenCopyBuffer, ci.ptScreenPos.x-10, ci.ptScreenPos.y-10, ci.hCursor, 0, 0, 0, NULL, DI_NORMAL | DI_COMPAT | DI_DEFAULTSIZE);

    GdiFlush();
  }

  if (capture_mode==MODE_DIRECTX) { DBGINT;
//    if (imageData==NULL)
//      imageData=malloc(dWidth*dHeight*BITSPERPIXEL/8);




// remove flicker effect
  errcheck(IDirect3DDevice9_Clear(d3dcapturedevice, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255,0,0), 1, 0));
  errcheck(IDirect3DDevice9_BeginScene(d3dcapturedevice));

  errcheck(IDirect3DDevice9_GetBackBuffer(d3dcapturedevice, 0, 0, D3DBACKBUFFER_TYPE_MONO, &pDestTarget));
  errcheck(IDirect3DSurface9_LockRect(pDestTarget, &pLockedRect, NULL, D3DLOCK_DISCARD));
  for(i=0 ; i < dHeight ; i++) { DBGINT;
    memcpy( (BYTE*) pLockedRect.pBits + (dHeight-i-1) * pLockedRect.Pitch, desktopBitmapData + i * ((((dWidth * 32) + 31) & ~31) >> 3), dWidth * BITSPERPIXEL / 8);
  }
  errcheck(IDirect3DSurface9_UnlockRect(pDestTarget));

  IDirect3DSurface9_Release(pDestTarget);

  errcheck(IDirect3DDevice9_EndScene(d3dcapturedevice));


    errcheck(IDirect3DDevice9_CreateOffscreenPlainSurface(d3dcapturedevice, dWidth, dHeight, d3ddm.Format, D3DPOOL_SYSTEMMEM, &pCopySurface, NULL));
    errcheck(IDirect3DDevice9_Present(d3dcapturedevice, NULL, NULL, NULL, NULL));
    errcheck(IDirect3DDevice9_GetRenderTarget(d3dcapturedevice, 0, &pRenderTarget));
    errcheck(IDirect3DDevice9_GetRenderTargetData(d3dcapturedevice, pRenderTarget, pCopySurface)); 
    errcheck(IDirect3DSurface9_Release(pRenderTarget));
    errcheck(IDirect3DSurface9_LockRect(pCopySurface, &pLockedRect, NULL, D3DLOCK_NO_DIRTY_UPDATE | D3DLOCK_NOSYSLOCK | D3DLOCK_READONLY));
    for(i=0 ; i < dHeight ; i++) { DBGINT;
      memcpy( desktopBitmapData + (dHeight-i-1) * ((((dWidth * 32) + 31) & ~31) >> 3), (BYTE*) pLockedRect.pBits + i* pLockedRect.Pitch, dWidth * BITSPERPIXEL / 8);
    }
    errcheck(IDirect3DSurface9_UnlockRect(pCopySurface));
    errcheck(IDirect3DSurface9_Release(pCopySurface));

    errcheck(IDirect3DDevice9_Present(d3dcapturedevice, NULL, NULL, NULL, NULL));
  }

  benchmark_range(alabel,BM_STOP,&a);
  benchmark_range(blabel,BM_START,&a);

  if (scale) { DBGINT;
    if (!cCtx[0]) { DBGINT;
      avpicture_alloc(&converted, PIX_FMT_BGRA, wWidth, wHeight);

      initScaleCtx();

      if (!sslEngaged) { DBGINT;
        EnterCriticalSection(&scalestartlock);
        sslEngaged=TRUE;
      }

      for (i=0; i<4; i++)
        if (scale_threads[i]==NULL)
          scale_threads[i]=CreateThread(NULL, 0, scaleloop, (LPVOID) i, 0, NULL);

      reinitscaling=FALSE;
    } else if (reinitscaling) { DBGINT;
      initScaleCtx();
      reinitscaling=FALSE;
    }

    while ((*(volatile int *)&scalesComplete)!=4)
      Sleep(0);
    MEMORY_BARRIER;
    scalesComplete=0;
    EnterCriticalSection(&scalestoplock);
    LeaveCriticalSection(&scalestartlock);
    while ((*(volatile int *)&scalesComplete)!=4)
      Sleep(0);
    MEMORY_BARRIER;
    scalesComplete=0;
    EnterCriticalSection(&scalestartlock);
    LeaveCriticalSection(&scalestoplock);
  } else if (reinitscaling) { DBGINT;
    initScaleCtx();
    reinitscaling=FALSE;
  }

  benchmark_range(blabel,BM_STOP,&a);
  benchmark_range(clabel,BM_START,&a);

  rframe.size = x264_encoder_encode(x264encoder, &nals, &i_nals, &pic_in, &pic_out);

//  x264_encoder_intra_refresh(x264encoder); start intra-refresh
//    void (*nalu_process) ( x264_t *h, x264_nal_t *nal, void *opaque ); need this for low-latency encoding
  if (rframe.size < 0) { DBGINT;
    MsgBox("Error on encoding frame.");
    SafeShutdown();
  }

//pic_out.b_keyframe
//  sprintf(frameStream+strlen(frameStream),(IS_X264_TYPE_I(pic_out.i_type) ? "I" : (X264_TYPE_P==pic_out.i_type ? "P" : "B")));
  avgcrf+=pic_out.prop.f_crf_avg;
//  rframe.elements=i_nals;
  rframe.data=(BYTE *)nals;

  benchmark_range(clabel,BM_STOP,&a);

  return &rframe;
}

void release_audio_variables(void) { DBGINT;
  int i;

  if (waveOut!=NULL) { DBGINT;
    if (waveOutReset(waveOut)!=MMSYSERR_NOERROR) { DBGINT;
      Alert("Could not reset waveOut device.");
      SafeShutdown();
    }

    for (i=0; i<WAVEOUT_HEADERS; i++)
      if (wohdrs[i].state==PREPARED) { DBGINT;
        Sleep(0);
        i--;
      }

    MEMORY_BARRIER; // read-acquire

    if (waveOutClose(waveOut)!=MMSYSERR_NOERROR) { DBGINT;
      Alert("Could not close waveOut device.");
      SafeShutdown();
    }

    i=0;

    MEMORY_BARRIER; // write-release

    QueueUserAPC(exitWorkerAPC, process_audio_thread, 0);
//    QueueUserAPC(exitWorkerAPC, process_audio_thread, (ULONG_PTR)&i);
//    while ((*(volatile int *)&i)==0) Sleep(0);
//    MEMORY_BARRIER; // read-acquire

    audio_playback_active=FALSE;

    waveOut=NULL;

    opus_decoder_destroy(adec);
    CloseHandle(process_audio_thread);
    cb_free(playBuffer);

    free(wohdrs);
  }

  if (waveIn!=NULL) { DBGINT;
    EnterCriticalSection(&paAPClock);
    audio_recording_active=FALSE;
    LeaveCriticalSection(&paAPClock);


    if (waveInReset(waveIn)!=MMSYSERR_NOERROR) { DBGINT;
      Alert("Could not reset wave device.");
      SafeShutdown();
    }

    for (i=0; i<WAVEINHDRS; i++)
      if (whdrs[i].state==PREPARED) { DBGINT;
        Sleep(0);
        i--;
      }

    MEMORY_BARRIER; // read-acquire

    QueueUserAPC(exitWorkerAPC, process_audio_thread, 0);

//    i=0;
//    MEMORY_BARRIER; // write-release
//    QueueUserAPC(exitWorkerAPC, process_audio_thread, (ULONG_PTR)&i);
//    while ((*(volatile int *)&i)==0) Sleep(0); // wait for recording thread to close. this must happen before the waveIn device is closed so we can unprepare all the headers.
//    MEMORY_BARRIER; // read-acquire

    opus_encoder_destroy(aenc);
    aenc=NULL;

    if ((i=waveInClose(waveIn))!=MMSYSERR_NOERROR) { DBGINT;
      Alert("Could not close waveIn device. %i", i);
      SafeShutdown();
    }
    waveIn=NULL;
    CloseHandle(process_audio_thread);

    EnterCriticalSection(&(abuffer->lock));
    abuffer->blocks=FALSE;

    SetEvent(abuffer->event);
    LeaveCriticalSection(&(abuffer->lock));

    EnterCriticalSection(&nbslock);
    LeaveCriticalSection(&nbslock);

    cb_free(abuffer);

    CloseHandle(nonblocking_send_thread);
  }
}

void release_allocated_variables(void) { DBGINT;
  int i;

  if (hDCScreen!=NULL) { DBGINT;
//    pic_in.img.plane[0]=oldImgPtr;
//    x264_picture_clean(&pic_in);
//    x264_picture_clean(&pic_out);
    x264_encoder_close(x264encoder);
    x264encoder=NULL;
    SelectObject(hDCScreenCopyBuffer,oldBitmap);
    DeleteDC (hDCScreenCopyBuffer);
//    ReleaseDC (aDesktop, hDCScreen);
    DeleteDC(hDCScreen);
    DeleteObject (aBitmap);
    hDCScreen=NULL;
    if (cCtx[0]) { DBGINT;
      avpicture_free(&converted);
      for (i=0; i<4; i++) { DBGINT;
        sws_freeContext(cCtx[i]);
        cCtx[i]=NULL;
      }
    }
  }

  if (windowBitmap!=NULL) { DBGINT;
    SelectObject(hdcWindowPrebuffer,oldBitmap);
    DeleteObject(windowBitmap);
    DeleteDC (hdcWindowPrebuffer);
    windowBitmap=NULL;
  }

  if (avCodecContext!=NULL) { DBGINT;
    avcodec_close(avCodecContext);
    avCodecContext=NULL;
    av_free(picture);
    if (img_convert_ctx) { DBGINT;
      avpicture_free(&converted);
      sws_freeContext(img_convert_ctx);
      img_convert_ctx=NULL;
    }  
  }
}

enum PixelFormat fetchBGRA(struct AVCodecContext *s, const enum PixelFormat * fmt) { DBGINT;
  int i;
  volatile_buf[0]=0;
  while (fmt[i]!=-1)
    sprintf(volatile_buf+strlen(volatile_buf)," %i",fmt[i]);
  Alert(volatile_buf);
  exit(0);
  return 0;
}

int avcodec_init=0; 

BOOL fmtSleep;

DWORD WINAPI pixfmtloop(LPVOID in) {
  unsigned int x1, x2, y, y2, tWidth, remainder;
  AVPicture src={0},dst={0};
  static BOOL warned=FALSE;

  while (TRUE) {
    EnterCriticalSection(&fmtstartlock);
    LeaveCriticalSection(&fmtstartlock);
    if (img_convert_ctx == NULL) {
      CloseHandle(fmt_threads[(int)in]);
      fmt_threads[(int)in]=NULL;
      if (!warned && (((int)dst.data[0])%4 || ((int)src.data[0])%4 || ((int)src.data[1])%4 || ((int)src.data[2])%4)) {
        warned=TRUE;
        MAlert("Frame data was unaligned - %i %i %i %i", ((int)dst.data[0])%4, ((int)src.data[0])%4, ((int)src.data[1])%4, ((int)src.data[2])%4);
      }
      ExitThread(0);
    }

    dst=converted;
    src=*((AVPicture *)picture);
    dst.data[0]=((BYTE *)dst.data[0])+dst.linesize[0]*wHeight*((int)in)/4;
    src.data[0]=((BYTE *)src.data[0])+src.linesize[0]*wHeight*((int)in)/4;
    src.data[1]=((BYTE *)src.data[1])+src.linesize[1]*wHeight*((int)in)/4;
    src.data[2]=((BYTE *)src.data[2])+src.linesize[2]*wHeight*((int)in)/4;

/*
    for (y=wHeight/4; y--; ) {
      y2=y*dst.linesize[0];
      x1=wWidth*4+4;
      x2=wWidth+y*src.linesize[0]-1; //linesize[0] linesize[1] linesize[2] if we wanted to be official

      for (;x1-=4;x2--) {
        ((BYTE*)dst.data[0])[x1+y2-3]=*(((BYTE*)src.data[0])+x2); // green
        ((BYTE*)dst.data[0])[x1+y2-4]=*(((BYTE*)src.data[1])+x2); // blue
        ((BYTE*)dst.data[0])[x1+y2-2]=*(((BYTE*)src.data[2])+x2); // red
      }
    }
*/
    y=wHeight/4;
    remainder=wWidth%4;
    tWidth=(wWidth-remainder)/4;
    asm volatile(
         "push %%ebp\n\t"

         "movd %%esp,%%mm2\n\t"

         "mov %2,%%esp\n\t"
         "dec %%ecx\n\t"
         "imul %%ecx,%%esp\n\t" // calculated y2 in esp
         "add %4,%%esp\n\t" // esp now stores dst.data[0] pointer with offset y2
         "mov %1,%%eax\n\t"
         "leal (%%esp,%%eax,4), %%esp\n\t" // displacement(base register, offset register, scalar multiplier). esp now stores dst.data[0] pointer with offset y2 + wWidth*4+4
         "sub $4,%%esp\n\t" // %%esp first destination address -- ((BYTE*)dst.data[0])+wHeight/4*dst.linesize[0]-4 (got rid of +wWidth*4 since we're using ecx,4 as the index/scalar offset) (then added wwidth+4 on :P)

         "mov %3,%%edx\n\t"
//         "sub %%ebx,%%esi\n\t" // calculated src padding in esi based on src.linesize
"imul $4,%%eax\n\t"
         "mov %2,%%ebx\n\t"
"sub %%eax,%%ebx\n\t" // calculated dst padding in edi based on dst.linesize
//"add $8,%%ebx\n\t" // calculated dst padding in edi based on dst.linesize

         "mov %1,%%eax\n\t"
         "movd %%eax,%%mm7\n\t" // mm7 has wWidth
         "mov %9,%%eax\n\t"
         "movd %%eax,%%mm0\n\t" // mm0 has tWidth
         "mov %10,%%eax\n\t"
         "movd %%eax,%%mm6\n\t" // mm6 has remainder

         "mov %3,%%eax\n\t"
         "imul %%ecx,%%eax\n\t"
//         "add %10,%%eax\n\t"
         "sub $4,%%eax\n\t" // %%eax source offset (x2)
         "mov %5,%%edi\n\t" // src.data[0] 
         "mov %6,%%esi\n\t" // src.data[1] 
         "mov %7,%%ebp\n\t" // src.data[2]
         "add %%eax,%%edi\n\t" // src.data[0] ptr + x2
         "add %%eax,%%esi\n\t" // src.data[1] ptr + x2
         "add %%eax,%%ebp\n\t" // src.data[2] ptr + x2

         "movd %%edx,%%mm3\n\t"
         "movd %%ebx,%%mm4\n\t"

         "inc %%ecx\n\t" // ecx has wHeight/4+1 or y+1
         "hLoop:\n\t"
           "movd %%ecx,%%mm1\n\t"

           "movd %%mm6,%%ecx\n\t"

           "cmp $0,%%ecx\n\t"
           "je endLine\n\t"

           "movd %%mm7,%%ebx\n\t"
           "mLoop:\n\t"
             "dec %%ebx\n\t"
             "movb 4(%%ebp,%%ebx),%%AH\n\t"  // src.data[2]+wWidth+y*src.linesize[0]-1
             "bswap %%eax\n\t"
             "movb 4(%%esi,%%ebx),%%AL\n\t" // src.data[1]+wWidth+y*src.linesize[0]-1
             "movb 4(%%edi,%%ebx),%%AH\n\t" // src.data[0]+wWidth+y*src.linesize[0]-1
             "mov %%eax,(%%esp)\n\t"
             "sub $4,%%esp\n\t"
           "loop mLoop\n\t"

           "endLine:\n\t"

           "movd %%mm0,%%ecx\n\t"
           "wLoop:\n\t"
             "movd %%ecx,%%mm5\n\t"

             "mov (%%ebp,%%ecx,4),%%ebx\n\t"  // src.data[2]+wWidth+y*src.linesize[0]-1
             "mov (%%edi,%%ecx,4),%%edx\n\t" // src.data[0]+wWidth+y*src.linesize[0]-1
             "mov (%%esi,%%ecx,4),%%ecx\n\t" // src.data[1]+wWidth+y*src.linesize[0]-1

             "movb %%BL,%%AH\n\t"  // src.data[2]+wWidth+y*src.linesize[0]-1
//"movb $0x00,%%AL\n\t" // nothing
//"movb $0x00,%%AH\n\t" // red
             "bswap %%eax\n\t"
             "movb %%CL,%%AL\n\t" // src.data[1]+wWidth+y*src.linesize[0]-1
             "movb %%DL,%%AH\n\t" // src.data[0]+wWidth+y*src.linesize[0]-1
//"movb $0xFF,%%AL\n\t" // blue
//"movb $0x00,%%AH\n\t" // green - BGRA to GBRP
             "mov %%eax,-12(%%esp)\n\t"


             "movb %%BH,%%AH\n\t"  // src.data[2]+wWidth+y*src.linesize[0]-1
             "bswap %%eax\n\t"
             "movb %%CH,%%AL\n\t" // src.data[1]+wWidth+y*src.linesize[0]-1
             "movb %%DH,%%AH\n\t" // src.data[0]+wWidth+y*src.linesize[0]-1
             "mov %%eax,-8(%%esp)\n\t"

             "bswap %%ebx\n\t"
             "bswap %%ecx\n\t"
             "bswap %%edx\n\t"

             "movb %%BH,%%AH\n\t"  // src.data[2]+wWidth+y*src.linesize[0]-1
             "bswap %%eax\n\t"
             "movb %%CH,%%AL\n\t" // src.data[1]+wWidth+y*src.linesize[0]-1
             "movb %%DH,%%AH\n\t" // src.data[0]+wWidth+y*src.linesize[0]-1
             "mov %%eax,-4(%%esp)\n\t"

             "movb %%BL,%%AH\n\t"  // src.data[2]+wWidth+y*src.linesize[0]-1
             "bswap %%eax\n\t"
             "movb %%CL,%%AL\n\t" // src.data[1]+wWidth+y*src.linesize[0]-1
             "movb %%DL,%%AH\n\t" // src.data[0]+wWidth+y*src.linesize[0]-1
             "mov %%eax,(%%esp)\n\t"

             "movd %%mm5,%%ecx\n\t"

             "sub $16,%%esp\n\t"
//             "prefetchnta -1000(%%edx,%%ecx)\n\t"
           "loop wLoop\n\t"

           "movd %%mm3,%%edx\n\t"
           "movd %%mm4,%%ebx\n\t"
           "sub %%ebx,%%esp\n\t" // subtract padding from ebx (go up a line)
           "sub %%edx,%%edi\n\t" // subtract padding from ebx (go up a line)
           "sub %%edx,%%esi\n\t" // subtract padding from ebx (go up a line)
           "sub %%edx,%%ebp\n\t" // subtract padding from ebx (go up a line)
           "movd %%mm1,%%ecx\n\t"
           "dec %%ecx\n\t"
         "jnz hLoop\n\t"

         "movd %%mm2,%%esp\n\t"
         "pop %%ebp\n\t"

         :"=c"(y)         // no output
         :"m"(wWidth),"m"(dst.linesize[0]),"m"(src.linesize[0]),"m"(dst.data[0]),"m"(src.data[0]),"m"(src.data[1]),"m"(src.data[2]),"c"(y),"m"(tWidth),"m"(remainder)  // lots of inputs
         :"eax","ebx","edx","esi","edi"
    );

    EnterCriticalSection(&fmtcounterlock);
    fmtComplete++;
    LeaveCriticalSection(&fmtcounterlock);
    EnterCriticalSection(&fmtstoplock);
    LeaveCriticalSection(&fmtstoplock);
    EnterCriticalSection(&fmtcounterlock);
    fmtComplete++;
    LeaveCriticalSection(&fmtcounterlock);
  }
}

/*
void my_log_callback(void *ptr, int level, const char *fmt, va_list vargs)
{
sprintf(buf,fmt, vargs);
MessageBox(mainWindow,buf,"Alert",MB_OK);
}
*/

BOOL decode_frame(uint8_t *inbuf,int size, datablock *rframe) { DBGINT; // decode and return last frame from inbuf. initializes avcodec if not done so already.
    static AVCodec *codec=NULL;
    static AVPacket avpkt;
    SECURITY_ATTRIBUTES secattrib;
    int got_picture, len;
    RECT rect;
    const char *alabel="Decode";
    const char *blabel="PixFmt";
    clock_t a;

    benchmark_range(alabel,BM_START,&a);
    
    if (size==0) return;

    rframe->data=NULL;
    rframe->size=0;

    if (!avcodec_init) { DBGINT;
      avcodec_init=1;
//      av_log_set_level(AV_LOG_ERROR);
      printf("INFO: avcodec-55.dll should be compiled with MAX_SLICES set to 256 or it will dump a ton of warnings and degrade performance.");
      avcodec_register_all();
      codec = avcodec_find_decoder(AV_CODEC_ID_H264);
      if (!codec) { DBGINT;
        Alert("codec not found");
        SafeShutdown();
      }
    }
//av_log_set_callback(my_log_callback);
    if (avCodecContext==NULL) { DBGINT;
      av_init_packet(&avpkt);
      avCodecContext = avcodec_alloc_context3(codec);
      picture=avcodec_alloc_frame();

      if(codec->capabilities&CODEC_CAP_TRUNCATED)
        avCodecContext->flags|= CODEC_FLAG_TRUNCATED; /* we do not send complete frames */
//      avCodecContext->get_format=&fetchBGRA;

    /* For some codecs, such as msmpeg4 and mpeg4, width and height
       MUST be initialized there because this information is not
       available in the bitstream. */

     avCodecContext->thread_count=0; // auto multithreaded decoding
     avCodecContext->thread_type=FF_THREAD_SLICE; // split the decoding job of one thread up, don't try to decode multiple threads at once (slow)
//     avCodecContext->pix_fmt=PIX_FMT_BGRA;

    /* open it */
      if (avcodec_open2(avCodecContext, codec, NULL) < 0) { DBGINT;
        Alert("could not open context");
        SafeShutdown();
      }
    }

    avpkt.size = size;
    avpkt.data = inbuf;
    while (avpkt.size > 0) { DBGINT;
      len = avcodec_decode_video2(avCodecContext, picture, &got_picture, &avpkt);

      if (len < 0) { DBGINT;
//        Alert("Error while decoding frame");
        return FALSE;
      }
      if (len>0) { DBGINT;
//        rframe->data=(BYTE *)picture->data[0];
//        rframe->size=picture->linesize[0];
//        MsgBox("%i x %i x %i", picture->format, picture->width, picture->height);
//        MsgBox("%i", c->pix_fmt);
//        MsgBox(x264log);
//exit(0);
      }
      avpkt.size -= len;
      avpkt.data += len;
    }

    if (got_picture==0) return FALSE;

    if (img_convert_ctx == NULL) { DBGINT;
      wWidth=avCodecContext->width;
      wHeight=avCodecContext->height;

      GetClientRect(mainWindow,&rect);
      if (!fullscreen && rect.right-rect.left!=wWidth && rect.bottom-rect.top!=wHeight) { DBGINT;
        serverResized=TRUE;
        resizeMainWindow(wWidth,wHeight);
      }
      avpicture_alloc(&converted, PIX_FMT_BGRA, wWidth, wHeight);

      img_convert_ctx = sws_getContext(avCodecContext->width, avCodecContext->height, avCodecContext->pix_fmt, wWidth, wHeight, PIX_FMT_BGRA, SWS_FAST_BILINEAR, NULL, NULL, NULL);

      if (img_convert_ctx == NULL) { DBGINT;
        MsgBox("Cannot initialize the conversion context");
        SafeShutdown();
      }

      if (!fcsEngaged) { DBGINT;
        EnterCriticalSection(&fmtstartlock);
        fcsEngaged=TRUE;
      }
      for (len=0; len<4; len++)
        if (fmt_threads[len]==NULL) { DBGINT;
          fmt_threads[len]=CreateThread(NULL, 0, pixfmtloop, (LPVOID) len, 0, NULL);
        }
    }
    benchmark_range(alabel,BM_STOP,&a);
    benchmark_range(blabel,BM_START,&a);
    if (avCodecContext->pix_fmt!=PIX_FMT_BGRA) { DBGINT;
      if (avCodecContext->pix_fmt!=PIX_FMT_GBRP)
        sws_scale(img_convert_ctx, (const uint8_t *const *)picture->data, picture->linesize, 0, avCodecContext->height, converted.data, converted.linesize);
      else { // works faster than sws_scale when -Ofast is turned on
        while ((*(volatile int *)&fmtComplete)!=4)
          Sleep(0);
        MEMORY_BARRIER;
        fmtComplete=0;
        EnterCriticalSection(&fmtstoplock);
        LeaveCriticalSection(&fmtstartlock);
        while ((*(volatile int *)&fmtComplete)!=4)
          Sleep(0);
        MEMORY_BARRIER;
        fmtComplete=0;
        EnterCriticalSection(&fmtstartlock);
        LeaveCriticalSection(&fmtstoplock);
      }
      rframe->data=(BYTE *)converted.data[0];
      rframe->size=converted.linesize[0];
    } else {
      Debug("No conversion necessary!");
      rframe->data=(BYTE *)picture->data[0];
      rframe->size=picture->linesize[0];
    }
    benchmark_range(blabel,BM_STOP,&a);
    return TRUE;
}


VOID CALLBACK waveOutPauseAPC(ULONG_PTR dwParam) { DBGINT;
  if (waveOutPause(waveOut)!=MMSYSERR_NOERROR) { DBGINT;
    Alert("Could not pause waveOut device.");
    SafeShutdown();
  }
  audio_playback_paused=TRUE;
}

#define E_UNSPEC -1

void process_command(cmdStruct *cmd, int length) { DBGINT;
  x264_param_t param;
  HBITMAP hBmp;
  HRESULT res;
  BITMAPINFO bi;
  cmdStruct ocmd;
  INPUT input;
  clock_t oLow,oHigh,cTime;
  int i,j;
  static HGLOBAL memHandle=NULL;
  static int memPos=0,memLen=0;
  char *memPtr;
  static BYTE *fPtr=NULL;
  datalist *dlist,*xlist;

  switch (cmd->cmd) { DBGINT;
    case CMD_RESTARTSTREAM: // seen by client
      EnterCriticalSection(&updatewinlock); // don't decode any frames while resizing.
        release_allocated_variables();
        start_x264_stream_display();
      LeaveCriticalSection(&updatewinlock);
      break;

    case CMD_CHANGESIZE: // seen by server
      if (cmd->param1 > 0 && cmd->param2>0 && hDCScreen!=NULL) { DBGINT;
        EnterCriticalSection(&encoderlock);
//        if (scale) { DBGINT;
          wWidth=cmd->param1;
          wHeight=cmd->param2;
          if (wHeight%4!=0) wHeight-=wHeight%4;
//        }
//        else {
//          wWidth=dWidth;
//          wHeight=dHeight;
//        }
        release_allocated_variables();
        get_x264_stream_input(&hDCScreen, &hDCScreenCopyBuffer, &aDesktop, &aBitmap, &desktopBitmapData);
        ocmd.cmd=CMD_RESTARTSTREAM;
        ocmd.param1=wWidth;
        ocmd.param2=wHeight;
        transmit(clientsocket,&ocmd,sizeof(ocmd),TRANSMIT_CMD);
        LeaveCriticalSection(&encoderlock);
      }
      break;

    case CMD_MOUSEMOVE:
      EnterCriticalSection(&mouselock);
      LeaveCriticalSection(&mouselock);
      if (cmd->param1>=0 && cmd->param1<=(scale ? wWidth : dWidth) && cmd->param2>=0 && cmd->param2<=(scale ? wHeight : dHeight))
        SetCursorPos((scale ? cmd->param1*dWidth/wWidth : cmd->param1),(scale ? cmd->param2*dHeight/wHeight : cmd->param2));
      break;

    case CMD_MOUSECLICK:
      if (LOWORD(cmd->param1)>=0 && LOWORD(cmd->param1)<=(scale ? wWidth : dWidth) && HIWORD(cmd->param1)>=0 && HIWORD(cmd->param1)<=(scale ? wHeight : dHeight)) { DBGINT;
        input.type=INPUT_MOUSE;
        input.mi.dx=(scale ? LOWORD(cmd->param1)*dWidth/wWidth : LOWORD(cmd->param1));
        input.mi.dy=(scale ? HIWORD(cmd->param1)*dHeight/wHeight : HIWORD(cmd->param1));
        input.mi.mouseData=0;
        input.mi.time=0;
        input.mi.dwExtraInfo=0;
        input.mi.dwFlags=MOUSEEVENTF_ABSOLUTE;
        switch (LOWORD(cmd->param2)) { DBGINT;
          case CMD_MOUSECLICK_LBUTTON:
            if (HIWORD(cmd->param2)==CMD_MOUSECLICK_DOWN) { DBGINT;
              input.mi.dwFlags|=MOUSEEVENTF_LEFTDOWN;
            }
            else if (HIWORD(cmd->param2)==CMD_MOUSECLICK_UP)
              input.mi.dwFlags|=MOUSEEVENTF_LEFTUP;
            else
              input.mi.dwFlags|=MOUSEEVENTF_LEFTUP;
            break;
          case CMD_MOUSECLICK_RBUTTON:
            if (HIWORD(cmd->param2)==CMD_MOUSECLICK_DOWN)
              input.mi.dwFlags|=MOUSEEVENTF_RIGHTDOWN;
            else if (HIWORD(cmd->param2)==CMD_MOUSECLICK_UP)
              input.mi.dwFlags|=MOUSEEVENTF_RIGHTUP;
            else
              input.mi.dwFlags|=MOUSEEVENTF_LEFTUP;
            break;
          case CMD_MOUSECLICK_MBUTTON:
            if (HIWORD(cmd->param2)==CMD_MOUSECLICK_DOWN)
              input.mi.dwFlags|=MOUSEEVENTF_MIDDLEDOWN;
            else if (HIWORD(cmd->param2)==CMD_MOUSECLICK_UP)
              input.mi.dwFlags|=MOUSEEVENTF_MIDDLEUP;
            else
              input.mi.dwFlags|=MOUSEEVENTF_LEFTUP;
            break;
          case CMD_MOUSECLICK_WHEEL:
            input.mi.mouseData=(INT16)HIWORD(cmd->param2);
            input.mi.dwFlags|=MOUSEEVENTF_WHEEL;
            break;
          default:
            input.mi.dwFlags|=MOUSEEVENTF_LEFTUP;
            break;
        }
        SendInput(1,&input,sizeof(INPUT));
      }
      break;
    case CMD_KEYDOWN:
    case CMD_KEYUP:
      input.type=INPUT_KEYBOARD;
      input.ki.wVk=cmd->param1;
      input.ki.wScan=LOWORD(cmd->param2);
      input.ki.dwFlags=(cmd->cmd == CMD_KEYUP ? KEYEVENTF_KEYUP | HIWORD(cmd->param2) : HIWORD(cmd->param2));
//      input.ki.wScan=0;
//      input.ki.dwFlags=(cmd->cmd == CMD_KEYUP ? KEYEVENTF_KEYUP | cmd->param2 : cmd->param2);
      input.ki.time=0;
      input.ki.dwExtraInfo=0;
      SendInput(1,&input,sizeof(INPUT));
      break;

    case CMD_REBUFFER:
      AUDIO_BUFFER=LIMIT(cmd->param1,20,800);
      AUDIO_BUFFER_UPPER_LIMIT=LIMIT(cmd->param2,AUDIO_BUFFER+60,860);

      if (audio_playback_active && (msec_enqueued < AUDIO_BUFFER) && !audio_playback_paused) { DBGINT;
        QueueUserAPC(waveOutPauseAPC, client_thread, (ULONG_PTR)NULL);
      }
      break;

    case CMD_TIMESYNC:
      if (cmd->param2==0) { DBGINT;
        ocmd.cmd=CMD_TIMESYNC;
        ocmd.param1=cmd->param1;
        ocmd.param2=(int)clock();
        transmit(clientsocket,&ocmd,sizeof(ocmd),TRANSMIT_CMD);
      } else {
        cTime=clock();
        avgClientLatency=(avgClientLatency*9+cTime-cmd->param1)/10;
        oLow=(clock_t)(cmd->param2-cTime); // - if my clock is ahead of remote, + if my clock is behind remote clock. clock()+offsetTime
        oHigh=(clock_t)(cmd->param2-cmd->param1);
        // Actual offsettime ranges from offsetTime-offsetTimeError to offsetTime+offsetTimeError
        if (!clientTimeInit) { DBGINT;
          clientTimeInit=TRUE;
          ctoLow=oLow;
          ctoHigh=oHigh;
        } else if (ctoLow!=ctoHigh) { DBGINT;
          ctoLow=LIMIT(oLow,ctoLow,ctoHigh);
          ctoHigh=LIMIT(oHigh,ctoLow,ctoHigh);
        }
        clientLatency=cmd->param2-(cmd->param1+(ctoLow+ctoHigh)/2);
      }
      break;
    case CMD_SETTCPWINDOW:
      if (cmd->param1==CMD_SETTCPWINDOW_SET)
        tcpThrottling=cmd->param2;
      else if (cmd->param1==CMD_SETTCPWINDOW_UNSET)
        tcpThrottling=0;
      break;
    case CMD_DRAGDROP:
      switch (cmd->param1) {
        case CMD_DRAGDROP_COMPLETE:
printf("done\r\n");
          EnterCriticalSection(&flistlock);
          deleteFileList(&file_list);
          LeaveCriticalSection(&flistlock);
          break;
        case CMD_DRAGDROP_SEND:
printf("send\r\n");
          if (cmd->param2>=file_counter) return;
          EnterCriticalSection(&flistlock);
          dlist=file_list;
          i=-1;
printf("send1\r\n");
          while (dlist) {
            if ((++i)==cmd->param2) {
printf("send2 %i %i\r\n", dlist->index, length);
              HANDLE ev[2]={socketEvent,((fileInfo*)dlist_dataptr(dlist))->event2};
              WaitForMultipleObjects(2,ev,FALSE,INFINITE);
              #define fInfoPtr ((fileInfo*)dlist_dataptr(dlist))
              if ((res=fInfoPtr->stream->lpVtbl->Write(fInfoPtr->stream, ((BYTE *)cmd)+sizeof(cmdStruct), length-sizeof(cmdStruct), (ULONG*)&j))!=S_OK) Alert("inbound write error 1: %i", res);
              if (j!=length-sizeof(cmdStruct)) Alert("inbound write error 2: %i %i", j, length-sizeof(cmdStruct));
              LARGE_INTEGER a; a.QuadPart=-j;
              fInfoPtr->stream->lpVtbl->Seek(fInfoPtr->stream,a,STREAM_SEEK_CUR,NULL);
              SetEvent(fInfoPtr->event1);
printf("send3\r\n");
              break;
            }
            dlist=dlist->next;
          }
          LeaveCriticalSection(&flistlock);
printf("esend\r\n");
          break;
        case CMD_DRAGDROP_GET:
printf("get %i %i\r\n", cmd->param2, file_counter);
          if (cmd->param2>=file_counter) return;
          EnterCriticalSection(&flistlock);
          dlist=file_list;
          i=-1;
          while (dlist) {
            if ((++i)==cmd->param2) {
              xlist=(datalist *)malloc(sizeof(datalist)+sizeof(fileInfo));
              xlist->offset=0;
              xlist->index=i;
              xlist->size=dlist->size;
              xlist->type=DATATYPE_FILE;
              xlist->next=NULL;
              ((fileInfo*)dlist_dataptr(xlist))->file=((fileInfo*)dlist_dataptr(dlist))->file;
              addTransferList(xlist);
              break;
            }
            dlist=dlist->next;
          }
          LeaveCriticalSection(&flistlock);
printf("eget\r\n");
          break;
        case CMD_DRAGDROP_START:
printf("start\r\n");
          if (cmd->param2>0) { DBGINT;
            if (fPtr) {
              free(fPtr);
              fPtr=NULL;
            }

            memPos=0;
            memLen=cmd->param2;
            if (memLen>MAX_CLIPBOARD_SIZE || !(fPtr=malloc(memLen))) {
              Debug("File List object was too large: %i bytes", memLen);
              return;
            }
printf("a %i ",flistlock.LockCount);
            memcpy(fPtr+memPos,((BYTE *)cmd)+sizeof(cmdStruct),LIMIT(length-sizeof(cmdStruct),0,memLen-memPos));
printf("%i\r\n",flistlock.LockCount);
            memPos=LIMIT(length-sizeof(cmdStruct),0,memLen-memPos);

            EnterCriticalSection(&ptclock);
            totalBytes+=LIMIT(length,0,memLen-memPos);
            LeaveCriticalSection(&ptclock);
          }
          else if (fPtr) { DBGINT;
printf("b %i",flistlock.LockCount);
            memcpy(fPtr+memPos,((BYTE *)cmd)+sizeof(cmdStruct),LIMIT(length-sizeof(cmdStruct),0,memLen-memPos));
printf("%i\r\n",flistlock.LockCount);
            memPos+=LIMIT(length-sizeof(cmdStruct),0,memLen-memPos);

            EnterCriticalSection(&ptclock);
            totalBytes+=LIMIT(length,0,memLen-memPos);
            LeaveCriticalSection(&ptclock);
          }
          if (memPos!=0 && memPos==memLen) { DBGINT;
            memPos=0;
            EnterCriticalSection(&flistlock);
            deleteFileList(&file_list);
            #define CheckData(i) if (memPos+i > memLen) { MAlert("File overflow."); free(fPtr); if (((fileInfo*)dlist_dataptr(dlist))->name) free(((fileInfo*)dlist_dataptr(dlist))->name); fPtr=NULL; return; }
            while (memPos<memLen) {
              dlist=(datalist *)malloc(sizeof(datalist)+sizeof(fileInfo));
              dlist->offset=0;
              dlist->type=DATATYPE_FILE;
              dlist->next=NULL;
              dlist->index=file_counter;
              ((fileInfo*)dlist_dataptr(dlist))->file=NULL;
              ((fileInfo*)dlist_dataptr(dlist))->event1=CreateEvent(NULL,FALSE,FALSE,NULL);
              ((fileInfo*)dlist_dataptr(dlist))->event2=CreateEvent(NULL,FALSE,FALSE,NULL);
              CheckData(sizeof(int));
              i=*(int*)(fPtr+memPos);
              memPos+=sizeof(int);
              if (i>32000) { MAlert("Filename overflow: %.*s", 100, fPtr+memPos); free(fPtr); fPtr=NULL; return; }
              ((fileInfo*)dlist_dataptr(dlist))->name=malloc(i+1);
              CheckData(i);
              memcpy(((fileInfo*)dlist_dataptr(dlist))->name,fPtr+memPos,i);
              ((fileInfo*)dlist_dataptr(dlist))->name[i]=0;
              memPos+=i;
              ((fileInfo*)dlist_dataptr(dlist))->file=NULL;
              CheckData(sizeof(WIN32_FILE_ATTRIBUTE_DATA));
              memcpy(&(((fileInfo*)dlist_dataptr(dlist))->attr),fPtr+memPos,sizeof(WIN32_FILE_ATTRIBUTE_DATA));
              dlist->size=(((fileInfo*)dlist_dataptr(dlist))->attr.nFileSizeHigh*(MAXDWORD+1))+((fileInfo*)dlist_dataptr(dlist))->attr.nFileSizeLow;

              memPos+=sizeof(WIN32_FILE_ATTRIBUTE_DATA);

              file_counter++;

              if (!file_list) {
                file_list=dlist;
                file_end=dlist;
              }
              else {
                file_end->next=dlist;
                file_end=dlist;
              }
            }
            LeaveCriticalSection(&flistlock);
            free(fPtr);
            fPtr=NULL;
            dragSourceAction=DRAGSOURCE_CONTINUE;
            SetTimer(mainWindow,564644,1,doDragDrop);
          }
printf("estart\r\n");
          break;
        case CMD_DRAGDROP_GIVEFEEDBACK:
          if ((cmd->param2 & DROPEFFECT_NONE) || (cmd->param2 & DROPEFFECT_LINK))
            lastStatus=DROPEFFECT_NONE;
          else
            lastStatus=DROPEFFECT_COPY;
          break;
        case CMD_DRAGDROP_DROP:
          dragSourceAction=DRAGSOURCE_DROP;
          break;
        case CMD_DRAGDROP_STOP:
          EnterCriticalSection(&flistlock);
          deleteFileList(&file_list);
          LeaveCriticalSection(&flistlock);
          lastStatus=DROPEFFECT_NONE;
          dragSourceAction=DRAGSOURCE_CANCEL;
          break;
      }
      break;
    case CMD_BANDWIDTH_GOAL:
      bandwidth_goal=cmd->param1;
      fps_goal=cmd->param2;
      break;
    case CMD_CLIPBOARD:
      if (cmd->param1==0) { DBGINT;
        if (OpenClipboard(mainWindow)) { DBGINT;
          ignoreClip=TRUE;
          EmptyClipboard();
          CloseClipboard();
        }
      }
      else if (cmd->param1==CF_TEXT || cmd->param1==TRANSMIT_CF_HTML || cmd->param1==TRANSMIT_CF_RTF || cmd->param1==CF_BITMAP) { DBGINT; // -1 = CF_HTML
        if (cmd->param2>0) { DBGINT;
          if (memHandle) {
            GlobalFree(memHandle);
            memHandle=NULL;
          }
          memPos=0;
          memLen=cmd->param2;
          if (memLen>MAX_CLIPBOARD_SIZE || !(memHandle=GlobalAlloc(GMEM_MOVEABLE,memLen))) {
            Debug("Clipboard object was too large: %i bytes", memLen);
            return;
          }
          if (memPtr=GlobalLock(memHandle)) { DBGINT;
            memcpy(memPtr+memPos,((BYTE *)cmd)+sizeof(cmdStruct),LIMIT(length-sizeof(cmdStruct),0,memLen-memPos));
            memPos=LIMIT(length-sizeof(cmdStruct),0,memLen-memPos);
            GlobalUnlock(memHandle);

            EnterCriticalSection(&ptclock);
            totalBytes+=LIMIT(length,0,memLen-memPos);
            LeaveCriticalSection(&ptclock);
          }
        }
        else if (memHandle && (memPtr=GlobalLock(memHandle))) { DBGINT;
          memcpy(memPtr+memPos,((BYTE *)cmd)+sizeof(cmdStruct),LIMIT(length-sizeof(cmdStruct),0,memLen-memPos));
          memPos+=LIMIT(length-sizeof(cmdStruct),0,memLen-memPos);
          GlobalUnlock(memHandle);

          EnterCriticalSection(&ptclock);
          totalBytes+=LIMIT(length,0,memLen-memPos);
          LeaveCriticalSection(&ptclock);
        }
        if (memPos!=0 && memPos==memLen) { DBGINT;
          if (OpenClipboard(mainWindow)) { DBGINT;
            if (cmd->param1==CF_BITMAP) {
              memPtr=GlobalLock(memHandle);
              hBmp=CreateCompatibleBitmap(mainhDC,((BITMAPINFOHEADER*)memPtr)->biWidth,((BITMAPINFOHEADER*)memPtr)->biHeight);
              SetDIBits(mainhDC, hBmp, 0, ((BITMAPINFOHEADER*)memPtr)->biHeight, memPtr+sizeof(BITMAPINFOHEADER), (BITMAPINFO*)memPtr, DIB_RGB_COLORS);
              GlobalUnlock(memHandle);
              GlobalFree(memHandle);
              ignoreClip=TRUE;
              MEMORY_BARRIER;
              SetClipboardData(CF_BITMAP,hBmp);
            }
            else {
              ignoreClip=TRUE;
              MEMORY_BARRIER;
              if (!SetClipboardData(cmd->param1==TRANSMIT_CF_HTML ? CF_HTML : (cmd->param1==TRANSMIT_CF_RTF ? CF_RTF : cmd->param1),memHandle))
                GlobalFree(memHandle);
            }
            CloseClipboard();
          }
          memHandle=NULL;
        }
      }
      break;
  }
}

void timesync(void) { DBGINT;
  struct cmdStruct cmd;

  if (sizeof(clock_t)!=sizeof(cmd.param1)) { DBGINT;
    Alert("Clock_t and int size mismatch?");
    return;
  }

  cmd.cmd=CMD_TIMESYNC;
  cmd.param1=(int)clock();
  cmd.param2=0;
  transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);
}

int recv_interrupted(SOCKET s, void *b, int len, int flags) { DBGINT;
  int received=0,i;

  while ((received+=(i=recv(s,(char *)b+received,len-received,flags))) != len) { DBGINT;
    if (i==0 || i==SOCKET_ERROR) { DBGINT;
      return i;
    }
  }
  return len;
}

#define HSLEN 100
#define PPLEN 100
gcry_sexp_t keygen;
gcry_cipher_hd_t cipher_enc,cipher_dec;

VOID CALLBACK init_gcrypt(ULONG_PTR dwParam) { DBGINT;
  static init=FALSE;

  if (encryption) { DBGINT;
    if (init) return;
    init=TRUE;
    EnterCriticalSection(&cryptlock);
    SetWindowText(hwndText,"Generating unique crypto key for server use ...");
    if (gec(gcry_sexp_new(&keygen,"(genkey (rsa (nbits 4:4096)))",29,0)))
      SafeShutdown();
    if (gec(gcry_pk_genkey(&keypair, keygen)))
      SafeShutdown();
    gcry_sexp_release(keygen);
    LeaveCriticalSection(&cryptlock);
    SetWindowText(hwndText,"Crypto key ready.");
  }
  EnableWindow(hwndListen, TRUE);
}

#define SERVER(codeblock) if (serversocket!=SOCK_DISCONNECTED) { DBGINT; codeblock }
#define CLIENT(codeblock) if (serversocket==SOCK_DISCONNECTED) { DBGINT; codeblock }
#define C_OR_S(clientblock,serverblock) (serversocket==SOCK_DISCONNECTED ? clientblock : serverblock)
#define LOCALVAR(vname) *(serversocket==SOCK_DISCONNECTED ? &vname##_ctos : &vname##_stoc)
#define REMOTEVAR(vname) *(serversocket!=SOCK_DISCONNECTED ? &vname##_ctos : &vname##_stoc)
#define FAIL_AUTH Sleep(1000); LeaveCriticalSection(&cryptlock); crypt_mode=CRYPT_SLEEP; MEMORY_BARRIER; crypt_state=CRYPT_FAIL; continue; // write-release. sleep is intended to limit brute force attacks.
#define MIN_ITER 1000
#define MAX_ITER 2000

void gcrypt_loop(void) { DBGINT;
  int i,j,tpassoffset,i_stoc,i_ctos;
  char ctr_stoc[16],ctr_ctos[16],salt_stoc[8],salt_ctos[8],handshake[HSLEN+sizeof(int)];
  char *tpass,*key,tbuf[5002];
  gcry_sexp_t data,ciphertext,pubkey;

  crypt_thread=OpenThread(THREAD_SET_CONTEXT, TRUE, GetCurrentThreadId());

  while (!*(volatile BOOL *)&encryption)
    SleepEx(INFINITE,TRUE);

  if (!gcry_check_version (GCRYPT_VERSION)) { DBGINT;
    Alert("libgcrypt version mismatch\n", stderr);
    SafeShutdown();
  }

  gcry_control (GCRYCTL_SUSPEND_SECMEM_WARN);
  gcry_control (GCRYCTL_INIT_SECMEM, 16384, 0);
  gcry_control (GCRYCTL_RESUME_SECMEM_WARN);
  gcry_control (GCRYCTL_USE_SECURE_RNDPOOL);
  
//        gcry_control (GCRYCTL_DISABLE_SECMEM, 0);
  gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);
  if (gec(gcry_cipher_open (&cipher_enc, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_CTR, GCRY_CIPHER_SECURE)))
    SafeShutdown();
  if (gec(gcry_cipher_open (&cipher_dec, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_CTR, GCRY_CIPHER_SECURE)))
    SafeShutdown();

  EnterCriticalSection(&cryptshutdownlock);
  while (cryptActive) { DBGINT;
    switch (*(volatile int *)&crypt_mode) { DBGINT;
      case CRYPT_SLEEP:
        SleepEx(INFINITE,TRUE);
        break;
      case CRYPT_AUTH:
        MEMORY_BARRIER; // read-acquire
        EnterCriticalSection(&cryptlock);

        CLIENT(
               gcry_randomize(LOCALVAR(salt),8,GCRY_VERY_STRONG_RANDOM);
               send(clientsocket,(const char *)LOCALVAR(salt),8,0);
        );
        SERVER(if (recv_interrupted(clientsocket, (char *)REMOTEVAR(salt), 8, 0)!=8) { DBGINT; FAIL_AUTH; });
        SERVER(
               gcry_randomize(LOCALVAR(salt),8,GCRY_VERY_STRONG_RANDOM);
               send(clientsocket,(const char *)LOCALVAR(salt),8,0);
        );
        CLIENT(if (recv_interrupted(clientsocket, (char *)REMOTEVAR(salt), 8, 0)!=8) { DBGINT; FAIL_AUTH; });
        if (memcmp(LOCALVAR(salt),REMOTEVAR(salt),8)==0) { DBGINT; FAIL_AUTH; }


        CLIENT(
               gcry_randomize(LOCALVAR(ctr),16,GCRY_VERY_STRONG_RANDOM);
               send(clientsocket,(const char *)LOCALVAR(ctr),16,0);
        );
        SERVER(if (recv_interrupted(clientsocket, (char *)REMOTEVAR(ctr), 16, 0)!=16) { DBGINT; FAIL_AUTH; });
        SERVER(
               gcry_randomize(LOCALVAR(ctr),16,GCRY_VERY_STRONG_RANDOM);
               send(clientsocket,(const char *)LOCALVAR(ctr),16,0);
        );
        CLIENT(if (recv_interrupted(clientsocket, (char *)REMOTEVAR(ctr), 16, 0)!=16) { DBGINT; FAIL_AUTH; });
        if (memcmp(LOCALVAR(ctr),REMOTEVAR(ctr),16)==0) { DBGINT; FAIL_AUTH; }


        CLIENT(
               gcry_randomize(&LOCALVAR(i),sizeof(int),GCRY_VERY_STRONG_RANDOM);
               LOCALVAR(i)=abs(LOCALVAR(i)) % (MAX_ITER-MIN_ITER+1) + MIN_ITER;
               send(clientsocket,(const char *)&LOCALVAR(i),sizeof(int),0);
        );
        SERVER(if (recv_interrupted(clientsocket, (char *)&REMOTEVAR(i), sizeof(int), 0)!=sizeof(int)) { DBGINT; FAIL_AUTH; });
        SERVER(
               gcry_randomize(&LOCALVAR(i),sizeof(int),GCRY_VERY_STRONG_RANDOM);
               LOCALVAR(i)=abs(LOCALVAR(i)) % (MAX_ITER-MIN_ITER+1) + MIN_ITER;
               send(clientsocket,(const char *)&LOCALVAR(i),sizeof(int),0);
        );
        CLIENT(if (recv_interrupted(clientsocket, (char *)&REMOTEVAR(i), sizeof(int), 0)!=sizeof(int)) { DBGINT; FAIL_AUTH; });

        // if (memcmp(&LOCALVAR(i),&REMOTEVAR(i),sizeof(i_stoc))==0) { DBGINT; FAIL_AUTH; } don't really need this to be unique...


        key=gcry_malloc_secure(32);

        /* Server and client salt, ctr, and even kdf iteration values should all be unique. If all of these were the same for two unique ciphertexts, theoretically could get the encryption bitstream by NOT(ciphertext 1 XOR ciphertext 2), then XOR it with either ciphertext to decrypt it. */
        /* cipher_enc is this program's outgoing encryption. cipher_dec is the remote program's incoming encryption */

        if (gec(gcry_kdf_derive(passphrase, passphrase_length, GCRY_KDF_ITERSALTED_S2K, PASSPHRASE_DIGEST_ALGO, LOCALVAR(salt), 8, LOCALVAR(i), 32, key)))
          SafeShutdown();
        if (gec(gcry_cipher_setkey(cipher_enc, key, 32)))
          SafeShutdown();
        if (gec(gcry_cipher_setctr(cipher_enc, LOCALVAR(ctr), 16)))
          SafeShutdown();


        if (gec(gcry_kdf_derive(passphrase, passphrase_length, GCRY_KDF_ITERSALTED_S2K, PASSPHRASE_DIGEST_ALGO, REMOTEVAR(salt), 8, REMOTEVAR(i), 32, key)))
          SafeShutdown();
        if (gec(gcry_cipher_setkey(cipher_dec, key, 32)))
          SafeShutdown();
        if (gec(gcry_cipher_setctr(cipher_dec, REMOTEVAR(ctr), 16)))
          SafeShutdown();

        gcry_free(key);
        CLIENT(
               memset(passphrase, 0, passphrase_length);
               passphrase_set=FALSE;
        );


        /* Server verifies client has same passphrase. */
        SERVER(
               gcry_randomize(handshake,HSLEN,GCRY_VERY_STRONG_RANDOM);
               if (gec(gcry_cipher_encrypt(cipher_enc, handshake, HSLEN, tbuf, HSLEN))) SafeShutdown();
               send(clientsocket,(const char *)tbuf,HSLEN,0);
        );
        CLIENT(
               if (recv_interrupted(clientsocket, (char *)tbuf, HSLEN, 0)!=HSLEN) { DBGINT; FAIL_AUTH; }
               if (gec(gcry_cipher_decrypt (cipher_dec, tbuf, HSLEN, NULL, 0))) SafeShutdown();
               if (gec(gcry_cipher_encrypt(cipher_enc, tbuf, HSLEN, NULL, 0))) SafeShutdown();
               i=send(clientsocket,(const char *)tbuf,HSLEN,0);
        );
        SERVER(
               if (recv_interrupted(clientsocket, (char *)tbuf, HSLEN, 0)!=HSLEN) { DBGINT; FAIL_AUTH; }
               if (gec(gcry_cipher_decrypt (cipher_dec, tbuf, HSLEN, NULL, 0))) SafeShutdown();
               if (memcmp(tbuf,handshake,HSLEN)!=0) { DBGINT; FAIL_AUTH; }
        );


        /* Client verifies server has same passphrase. */
        CLIENT(
               gcry_randomize(handshake,HSLEN,GCRY_VERY_STRONG_RANDOM);
               if (gec(gcry_cipher_encrypt(cipher_enc, handshake, HSLEN, tbuf, HSLEN))) SafeShutdown();
               send(clientsocket,(const char *)tbuf,HSLEN,0);
        );
        SERVER(
               if (recv_interrupted(clientsocket, (char *)tbuf, HSLEN, 0)!=HSLEN) { DBGINT; FAIL_AUTH; }
               if (gec(gcry_cipher_decrypt (cipher_dec, tbuf, HSLEN, NULL, 0))) SafeShutdown();
               if (gec(gcry_cipher_encrypt(cipher_enc, tbuf, HSLEN, NULL, 0))) SafeShutdown();
               i=send(clientsocket,(const char *)tbuf,HSLEN,0);
        );
        CLIENT(
               if (recv_interrupted(clientsocket, (char *)tbuf, HSLEN, 0)!=HSLEN) { DBGINT; FAIL_AUTH; }
               if (gec(gcry_cipher_decrypt (cipher_dec, tbuf, HSLEN, NULL, 0))) SafeShutdown();
               if (memcmp(tbuf,handshake,HSLEN)!=0) { DBGINT; FAIL_AUTH; }
        );


        /* Server sends public key. */
        SERVER(
               pubkey=gcry_sexp_find_token (keypair,"public-key", 10);
               j=i=gcry_sexp_sprint(pubkey, GCRYSEXP_FMT_CANON, tbuf, 5000);
               gcry_sexp_release(pubkey);
               if (gec(gcry_cipher_encrypt(cipher_enc, &j, sizeof(j), NULL, 0))) SafeShutdown();
               if (gec(gcry_cipher_encrypt(cipher_enc, tbuf, i, NULL, 0))) SafeShutdown();
               send(clientsocket,(const char *)&j,sizeof(j),0);
               send(clientsocket,(const char *)tbuf,i,0);
        );

        /* Client receives public key, then generates a new passphrase, encrypts it with public key, and sends it to server (link is still encrypted with baseline AES). */
        CLIENT(
              if (recv_interrupted(clientsocket, (char *)&i, sizeof(i), 0)!=sizeof(i)) { DBGINT; FAIL_AUTH; }
              if (gec(gcry_cipher_decrypt(cipher_dec, &i, sizeof(i), NULL, 0))) SafeShutdown();
              if (i < 1 || i > 5000) { DBGINT; FAIL_AUTH; }
              if (recv_interrupted(clientsocket, (char *)tbuf, i, 0)!=i) { DBGINT; FAIL_AUTH; }
              if (gec(gcry_cipher_decrypt (cipher_dec, tbuf, i, NULL, 0)))
                SafeShutdown();

              if (gec(gcry_sexp_new(&pubkey,tbuf,i,0)))
                SafeShutdown();

              tpass=gcry_malloc_secure(200);
              sprintf(tpass,"(data (flags pkcs1) (value %i:", PPLEN); tpassoffset=strlen(tpass);
              gcry_randomize(tpass+tpassoffset,PPLEN,GCRY_VERY_STRONG_RANDOM);
              sprintf(tpass+tpassoffset+PPLEN,"))");

              if (gec(gcry_sexp_new(&data,tpass,tpassoffset+PPLEN+2,0)))
                SafeShutdown();

              if (gec(gcry_pk_encrypt(&ciphertext, data, pubkey)))
                SafeShutdown();

              gcry_sexp_release(data);
              gcry_sexp_release(pubkey);

              data=gcry_sexp_find_token(ciphertext,"rsa", 3);
              j=i=gcry_sexp_sprint(data, GCRYSEXP_FMT_CANON, tbuf, 5000);
              gcry_sexp_release(ciphertext);
              gcry_sexp_release(data);

              if (gec(gcry_cipher_encrypt(cipher_enc, &j, sizeof(j), NULL, 0))) SafeShutdown();
              if (gec(gcry_cipher_encrypt(cipher_enc, tbuf, i, NULL, 0))) SafeShutdown();
              send(clientsocket,(const char *)&j,sizeof(j),0);
              send(clientsocket,(const char *)tbuf,i,0);
        );

        /* Server receives AES-encrypted public key-encrypted passphrase, decrypts it. */
        SERVER(
              if (recv_interrupted(clientsocket, (char *)&i, sizeof(i), 0)!=sizeof(i)) { DBGINT; FAIL_AUTH; }
              if (gec(gcry_cipher_decrypt(cipher_dec, &i, sizeof(i), NULL, 0))) SafeShutdown();
              if (i < 1 || i > 5000-26) { DBGINT; FAIL_AUTH; }

              if (recv_interrupted(clientsocket, (char *)tbuf+25, i, 0)!=i) { DBGINT; FAIL_AUTH; }
              if (gec(gcry_cipher_encrypt(cipher_dec, tbuf+25, i, NULL, 0))) SafeShutdown();

              memcpy(tbuf, "(7:enc-val (flags pkcs1) ", 25);
              memcpy(tbuf+25+i,")",1);
              if (gec(gcry_sexp_new(&ciphertext, tbuf, 26+i, 0)))
                SafeShutdown();

              if (gec(gcry_pk_decrypt(&data, ciphertext, keypair)))
                SafeShutdown();

              gcry_sexp_release(ciphertext);

              tpass=(char *)gcry_sexp_nth_data(data, 1, &i);
              if (memcmp(tpass,passphrase,(passphrase_length < i ? passphrase_length : i))==0) { DBGINT; gcry_sexp_release(data); FAIL_AUTH; } // need to make sure that the new passphrase is unique; if for some reason it's the same as the authentication passphrase (variable passphrase), decryption described above is technically possible
        );

        key=gcry_malloc_secure(32);

        /* Derive encryption and decryption keys from new passphrase, injects them into cipher_enc and _dec. */
        if (gec(gcry_kdf_derive(C_OR_S(tpass+tpassoffset,tpass), C_OR_S(PPLEN,i), GCRY_KDF_ITERSALTED_S2K, PASSPHRASE_DIGEST_ALGO, LOCALVAR(salt), 8, LOCALVAR(i), 32, key))) // ok to reuse salt, iteration, and ctr since key is new
          SafeShutdown();
        if (gec(gcry_cipher_setkey(cipher_enc, key, 32)))
          SafeShutdown();
        if (gec(gcry_cipher_setctr(cipher_enc, LOCALVAR(ctr), 16)))
          SafeShutdown();

        if (gec(gcry_kdf_derive(C_OR_S(tpass+tpassoffset,tpass), C_OR_S(PPLEN,i), GCRY_KDF_ITERSALTED_S2K, PASSPHRASE_DIGEST_ALGO, REMOTEVAR(salt), 8, REMOTEVAR(i), 32, key))) // ok to reuse salt, iteration, and ctr since key is new
          SafeShutdown();
        if (gec(gcry_cipher_setkey(cipher_dec, key, 32)))
          SafeShutdown();
        if (gec(gcry_cipher_setctr(cipher_dec, REMOTEVAR(ctr), 16)))
          SafeShutdown();

        gcry_free(key);

        /* Invalidate tpass. */
        SERVER(gcry_sexp_release(data););
        CLIENT(gcry_free(tpass););

        crypt_mode=CRYPT_SLEEP;

        MEMORY_BARRIER; // write-release

        crypt_state=CRYPT_SUCCESS;
        LeaveCriticalSection(&cryptlock);
        break;
    }
  }
  EnterCriticalSection(&cryptlock);
  if (gcry_sexp_length(keypair)>0)
    gcry_sexp_release(keypair);
  gcry_cipher_close(cipher_dec);
  gcry_cipher_close(cipher_enc);
  LeaveCriticalSection(&cryptlock);
  LeaveCriticalSection(&cryptshutdownlock);
}

#define UNLOCK_CRYPT (ULONG_PTR)0
#define LOCK_CRYPT (ULONG_PTR)1
VOID CALLBACK WakeThreadAPC(ULONG_PTR dwParam) { DBGINT; }
void WakeThread(HANDLE t) { DBGINT;
  QueueUserAPC(WakeThreadAPC, t, (ULONG_PTR)NULL);
}

VOID CALLBACK single_threaded_encrypt(ULONG_PTR dwParam) { DBGINT;
  while ((*(volatile int *)&crypt_state)!=CRYPT_PENDING)
    Sleep(0);
  MEMORY_BARRIER; // read-acquire  
  if (gec(gcry_cipher_encrypt(cipher_enc, ((datablock *)dwParam)->data, ((datablock *)dwParam)->size, NULL, 0)))
    SafeShutdown();
  MEMORY_BARRIER; // write-release
  crypt_state=CRYPT_SUCCESS;
}
VOID CALLBACK single_threaded_decrypt(ULONG_PTR dwParam) { DBGINT;
  while ((*(volatile int *)&crypt_state)!=CRYPT_PENDING)
    Sleep(0);
  MEMORY_BARRIER; // read-acquire
  if (gec(gcry_cipher_decrypt(cipher_dec, ((datablock *)dwParam)->data, ((datablock *)dwParam)->size, NULL, 0)))
    SafeShutdown();
  MEMORY_BARRIER; // write-release
  crypt_state=CRYPT_SUCCESS;
}
void encrypt(void *buf,int len) { DBGINT;
  datablock d;
  if (!encryption) return;
  EnterCriticalSection(&cryptlock);
  d.data=buf;
  d.size=len;
  MEMORY_BARRIER; // write-release
  crypt_state=CRYPT_PENDING;
  QueueUserAPC(single_threaded_encrypt, crypt_thread, (ULONG_PTR)&d);
  while ((*(volatile int *)&crypt_state)==CRYPT_PENDING)
    Sleep(0);
  MEMORY_BARRIER; // read-acquire
  LeaveCriticalSection(&cryptlock);
}
void decrypt(void *buf,int len) { DBGINT;
  datablock d;
  if (!encryption) return;
  EnterCriticalSection(&cryptlock);
  d.data=buf;
  d.size=len;
  MEMORY_BARRIER; // write-release
  crypt_state=CRYPT_PENDING;
  QueueUserAPC(single_threaded_decrypt, crypt_thread, (ULONG_PTR)&d);
  while ((*(volatile int *)&crypt_state)==CRYPT_PENDING)
    Sleep(0);
  MEMORY_BARRIER; // read-acquire
  LeaveCriticalSection(&cryptlock);
}

DWORD WINAPI serverloop(LPVOID in) { DBGINT;
  HMENU menu;
  char tbuf[100], *substr;
  struct cmdStruct cmd;
  struct sockaddr_in address;
  fd_set r;
  struct addrinfo *addrinfo;
  struct timeval timeout;
  int i,datasize=-1,datareceived;
  uint16_t port=10900;
  BOOL recvd,clientCleanup=FALSE;
  BYTE recvType;

  serversocket = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
  clientsocket = SOCK_DISCONNECTED;

  if (serversocket == INVALID_SOCKET) { DBGINT;
    serversocket=SOCK_DISCONNECTED;
    EnableWindow(hwndIP, TRUE);
    EnableWindow(hwndListen, TRUE);
    EnableWindow(hwndConnect, TRUE);
    SetWindowText(hwndListen, "Start Server");
    MsgBox("Cannot create server socket.");
    ExitThread(0);
  }

  GetWindowText(hwndIP,tbuf,100);
  substr=strstr(tbuf,":");
  if (substr) { DBGINT;
    port=atoi(substr+1);
    if (port < 1 || port > 65535)
      port=10900;
    *substr=0;
  }


  if (getaddrinfo(tbuf, NULL, NULL, &addrinfo)!=0) { DBGINT;
    serversocket=SOCK_DISCONNECTED;
    EnableWindow(hwndIP, TRUE);
    EnableWindow(hwndListen, TRUE);
    EnableWindow(hwndConnect, TRUE);
    SetWindowText(hwndListen, "Start Server");
    MsgBox("Cannot resolve hostname.");
    if (addrinfo) freeaddrinfo(addrinfo);
    ExitThread(0);
  }

  ZeroMemory(&address,sizeof(address));
  address.sin_family = AF_INET;
//  address.sin_addr.S_un.S_addr=((struct sockaddr_in *)addrinfo)->sin_addr.S_un.S_addr;
  if (strstr(tbuf,"127.0.0.1")==tbuf || strstr(tbuf,"localhost")==tbuf)
    address.sin_addr.S_un.S_addr=htonl(INADDR_ANY);
  else
    address = *(struct sockaddr_in *)(addrinfo->ai_addr);
  freeaddrinfo(addrinfo);
  address.sin_port = htons(port);

  if (bind(serversocket, (SOCKADDR*) &address, sizeof(address) ) == SOCKET_ERROR) { DBGINT;
    closesocket(serversocket);
    serversocket=SOCK_DISCONNECTED;
    EnableWindow(hwndIP, TRUE);
    EnableWindow(hwndListen, TRUE);
    EnableWindow(hwndConnect, TRUE);
    SetWindowText(hwndListen, "Start Server");
    MsgBox("Cannot bind server socket.");
    ExitThread(0);
  }

  if (listen(serversocket, 3) == SOCKET_ERROR) { DBGINT;
    closesocket(serversocket);
    serversocket=SOCK_DISCONNECTED;
    EnableWindow(hwndIP, TRUE);
    EnableWindow(hwndListen, TRUE);
    EnableWindow(hwndConnect, TRUE);
    SetWindowText(hwndListen, "Start Server");
    MsgBox("Cannot listen on server socket.");
    ExitThread(0);
  }

  FD_ZERO(&r);
  FD_SET(serversocket, &r);
  timeout.tv_sec=1;
  timeout.tv_usec=0;

  SetWindowText(hwndConnect, "Disconnected");
  SetWindowText(hwndListen, "Stop Server");

  AttachThreadInput(GetCurrentThreadId(),windowThreadId,TRUE);

  EnableWindow(hwndListen, TRUE);
  EnableWindow(hwndCRF, TRUE);
  EnableWindow(hwndFPS, TRUE);
  EnableWindow(hwndBITRATE, TRUE);
  EnableWindow(hwndABITRATE, TRUE);
  EnableWindow(hwndAC, TRUE);
  EnableWindow(hwndSCALE, TRUE);
  EnableWindow(hwndABL, TRUE);
  EnableWindow(hwndABM, TRUE);  
  EnableWindow(hwndFD, TRUE);
  EnableWindow(hwndSPEED, TRUE);
  EnableWindow(hwndACB, TRUE);
  EnableWindow(hwndAudioDevice, TRUE);

  menu=GetSystemMenu(mainWindow,FALSE);
  InsertMenu(menu,0,MF_BYPOSITION | MF_SEPARATOR,1235,NULL);
  InsertMenu(menu,0,MF_BYPOSITION | MF_ENABLED | MF_STRING,MENU_PASSWORD,"Change Password");

  taskIcon.uID=1;
  taskIcon.uVersion = NOTIFYICON_VERSION;
  taskIcon.cbSize=sizeof(taskIcon);
  taskIcon.hWnd=mainWindow;
  taskIcon.uFlags=NIF_ICON | NIF_TIP | NIF_MESSAGE;
  taskIcon.uCallbackMessage=TASKTRAY_MESSAGE;
  sprintf(taskIcon.szTip,"Stream Server");
  taskIcon.hIcon=LoadImage(shell32, MAKEINTRESOURCE(309), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
  Shell_NotifyIcon(NIM_ADD,&taskIcon);

  SetFocus(hwndListen);

  serverActive=TRUE;

  while (serverActive && select(0, &r, NULL, NULL, &timeout)!=SOCKET_ERROR) { DBGINT;
    if (clientsocket==SOCK_DISCONNECTED) { DBGINT;
      if (clientCleanup) { DBGINT;
        resetCursor();
        clientCleanup=FALSE;
      }
      if (FD_ISSET(serversocket, &r)) { DBGINT; // accept connections
        i=sizeof(struct sockaddr);
        clientsocket=accept(serversocket, (struct sockaddr *)&address, &i);
        datareceived=MAX_SOCKET_BUFFER_SIZE;
        i=sizeof(datareceived);
        setsockopt(clientsocket, SOL_SOCKET, SO_SNDBUF, (char *)&datareceived, i); // how much data to buffer before we start blocking because either the client isn't a fast enough decoder or the network is congested
        i=sizeof(datareceived);
        getsockopt(clientsocket, SOL_SOCKET, SO_SNDBUF, (char *)&datareceived, &i);
        if (datareceived!=MAX_SOCKET_BUFFER_SIZE) { DBGINT;
          MAlert("Could not set desired outbut socket buffer size (currently %i). Performance may be degraded.",datareceived);
        }
        datareceived=0;

        i=1;
        setsockopt(clientsocket, IPPROTO_TCP, TCP_NODELAY, (char *)&i, sizeof(int));
//        u_long nonblocking=1;
//        ioctlsocket(clientsocket,FIONBIO,&nonblocking);

        if (encryption) { DBGINT;
          SetWindowText(hwndConnect, "Authenticating");
          crypt_state=CRYPT_PENDING;
          MEMORY_BARRIER; // don't want the order of these variables getting moved around
          crypt_mode=CRYPT_AUTH;
          MEMORY_BARRIER; // don't want the order of these variables getting moved around
          WakeThread(crypt_thread);
          while ((*(volatile int *)&crypt_state)==CRYPT_PENDING)
            Sleep(100);
          MEMORY_BARRIER; // read-acquire

          if (crypt_state==CRYPT_FAIL) { DBGINT;
            closesocket(clientsocket);
            clientsocket=SOCK_DISCONNECTED;
            SetEvent(socketEvent);
            SetWindowText(hwndConnect, "Disconnected");
            continue;
          } else {
            clientActive=TRUE;
            CloseHandle(CreateThread(NULL, 0, screen_capture_loop, NULL, 0, NULL));
            SetWindowText(hwndConnect, "Connected");
          }
        }
        else {
          clientActive=TRUE;
          CloseHandle(CreateThread(NULL, 0, screen_capture_loop, NULL, 0, NULL));
          SetWindowText(hwndConnect, "Connected");
        }

        ResetEvent(socketEvent);

        clientCleanup=TRUE;
        cmd.cmd=CMD_REBUFFER;
        GetWindowText(hwndABL,tbuf,50);
        cmd.param1=atoi(tbuf);
        cmd.param1=LIMIT(cmd.param1,20,800);
        GetWindowText(hwndABM,tbuf,50);
        cmd.param2=atoi(tbuf);
        cmd.param2=LIMIT(cmd.param2,80,860);
        if (cmd.param1+60 > cmd.param2) cmd.param2=cmd.param1+60;
        transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);
        cmd.cmd=CMD_BANDWIDTH_GOAL;
        cmd.param1=bandwidth_goal;
        cmd.param2=fps_goal;
        transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);

      // MAKEIPADDRESS(address.sin_addr.S_un.S_un_b.s_b1, address.sin_addr.S_un.S_un_b.s_b2, address.sin_addr.S_un.S_un_b.s_b3, 0); // make an IP mask with the class D as 0
      }
      FD_ZERO(&r); // set sockets to poll
      FD_SET(serversocket, &r);
    } else {
      if (FD_ISSET(clientsocket, &r)) { DBGINT; // read client data
        if (datasize<0) { DBGINT;
          i=recv_interrupted(clientsocket, (char *)&datasize, sizeof(int), 0);
          if (i!=0 && i!=SOCKET_ERROR && i==sizeof(int) && encryption)
            decrypt(&datasize,sizeof(int));
          if (i==0 || i==SOCKET_ERROR || i!=sizeof(int) || datasize<1 || datasize>RECV_BUFFER_SIZE) { DBGINT;
            if (i!=SOCKET_ERROR && i!=0) { DBGINT;
              if (i!=4) { DBGINT;
                MAlert("Received data int of size %i. Disconnected.",i);
              }
              else if (datasize<1) { DBGINT;
                MAlert("Received data int of value %i. Disconnected.",datasize);
              }
              else if (datasize>RECV_BUFFER_SIZE) { DBGINT;
                MAlert("Received data int of value %i (max %i; would cause receive buffer overflow). Disconnected.",datasize,RECV_BUFFER_SIZE);
              }
            }
            closesocket(clientsocket);
            clientsocket=SOCK_DISCONNECTED;
            SetEvent(socketEvent);
            clientActive=FALSE;
            FD_ZERO(&r); // set sockets to poll
            FD_SET(serversocket, &r);
            SetEvent(socketEvent);
            SetWindowText(hwndConnect, "Disconnected");
            continue;
          }


          i=recv_interrupted(clientsocket, (char *)&recvType, sizeof(recvType), 0);
          if (i==0 || i==SOCKET_ERROR) { DBGINT;
            closesocket(clientsocket);
            clientsocket=SOCK_DISCONNECTED;
            SetEvent(socketEvent);
            clientActive=FALSE;
            FD_ZERO(&r); // set sockets to poll
            FD_SET(serversocket, &r);
            SetEvent(socketEvent);
            SetWindowText(hwndConnect, "Disconnected");
            continue;
          }
          if (encryption)
            decrypt(&recvType,sizeof(recvType));
        }

        i=recv_interrupted(clientsocket, recvbuffer+datareceived, datasize-datareceived, 0);
        if (i==0 || i==SOCKET_ERROR) { DBGINT;
          closesocket(clientsocket);
          clientsocket=SOCK_DISCONNECTED;
          SetEvent(socketEvent);
          clientActive=FALSE;
          FD_ZERO(&r); // set sockets to poll
          FD_SET(serversocket, &r);
          SetEvent(socketEvent);
          SetWindowText(hwndConnect, "Disconnected");
          continue;
        }
        datareceived+=i;

        if (datasize==datareceived) { DBGINT;
          if (encryption)
            decrypt(recvbuffer,datareceived);
          process_command((cmdStruct *)recvbuffer,datasize);
          datasize=-1;
          datareceived=0;
        }
      }
      FD_ZERO(&r); // set sockets to poll
      FD_SET(clientsocket, &r);
    }
  }

  if (clientCleanup) { DBGINT;
    resetCursor();
    clientCleanup=FALSE;
  }

  SetEvent(socketEvent);

  if (clientsocket!=SOCK_DISCONNECTED)
    closesocket(clientsocket);

  closesocket(serversocket);
  serversocket=clientsocket=SOCK_DISCONNECTED;
  SetEvent(socketEvent);

  Shell_NotifyIcon(NIM_DELETE,&taskIcon);
  DestroyIcon(taskIcon.hIcon);
  taskIcon.uID=0;

  GetSystemMenu(mainWindow,TRUE);

  clientActive=FALSE;
  serverActive=FALSE;
  SetWindowText(hwndListen, "Start Server");
  SetWindowText(hwndConnect, "Connect");
  EnableWindow(hwndIP, TRUE);
  EnableWindow(hwndCRF, FALSE);
  EnableWindow(hwndFPS, FALSE);
  EnableWindow(hwndBITRATE, FALSE);
  EnableWindow(hwndABITRATE, FALSE);
  EnableWindow(hwndAC, FALSE);
  EnableWindow(hwndSCALE, FALSE);
  EnableWindow(hwndListen, TRUE);
  EnableWindow(hwndConnect, TRUE);
  EnableWindow(hwndABL, FALSE);
  EnableWindow(hwndABM, FALSE);  
  EnableWindow(hwndFD, FALSE);  
  EnableWindow(hwndSPEED, FALSE);  
  EnableWindow(hwndACB, FALSE);  
  EnableWindow(hwndAudioDevice, FALSE);

  SetFocus(hwndListen);
  ExitThread(0);
}

DWORD WINAPI lowPriorityDataThread(LPVOID in) { DBGINT;
  int i,bandwidth_remaining;
  while (TRUE) {
    SleepEx(1000/fps_goal,TRUE);
    i=bandwidth_goal - totalSent; // remaining bytes that can be sent at this time

    if (i<0) {
      bandwidth_remaining+=i;
    }
    else if (bandwidth_remaining<0) {
      bandwidth_remaining=(i+=bandwidth_remaining);
    }

    if (bandwidth_remaining>0)
      bandwidth_remaining=0;

    i=LIMIT(i,0,RECV_BUFFER_SIZE);
    transferLowPriorityData(i);

    totalSent=0;
  }
}

HANDLE videoWorker,audioWorker,lowPriorityDataWorker;

DWORD WINAPI clientloop(LPVOID in) { DBGINT;
  HMENU menu;
  struct sockaddr_in address;
  fd_set r;
  struct timeval timeout;
  int i, j, datasize;
  BYTE recvType;
  static BOOL warned=FALSE;
  datablock ret;
  struct addrinfo *addrinfo;
  char tbuf[100], *substr;
  uint16_t port=10900;
  clock_t a;
  const char *alabel="Receive";
  const char *blabel="Decrypt";
  cmdStruct cmd;

  GetWindowText(hwndIP,tbuf,100);
  substr=strstr(tbuf,":");
  if (substr) { DBGINT;
    port=atoi(substr+1);
    if (port < 1 || port > 65535)
      port=10900;
    *substr=0;
  }


  if (getaddrinfo(tbuf, NULL, NULL, &addrinfo)!=0) { DBGINT;
    EnableWindow(hwndListen, TRUE);
    EnableWindow(hwndConnect, TRUE);
    MsgBox("Cannot resolve hostname.");
    if (addrinfo) freeaddrinfo(addrinfo);
    ExitThread(0);
  }

  clientsocket = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);

  if (clientsocket == INVALID_SOCKET) { DBGINT;
    clientsocket=SOCK_DISCONNECTED;
    EnableWindow(hwndListen, TRUE);
    EnableWindow(hwndConnect, TRUE);
    MsgBox("Cannot create client socket.");
    freeaddrinfo(addrinfo);
    ExitThread(0);
  }

  address.sin_family = AF_INET;
//  address.sin_addr.s_addr=inet_addr(((serveraddr *)params)->ip);
//  DWORD j=100;
//  WSAAddressToString(addrinfo->ai_addr, sizeof(*addrinfo->ai_addr), NULL, tbuf, &j);
  address = *(struct sockaddr_in *)(addrinfo->ai_addr);
  freeaddrinfo(addrinfo);
  address.sin_port = htons(port);

  if (connect( clientsocket, (SOCKADDR*) &address, sizeof(address) ) == SOCKET_ERROR) { DBGINT;
    closesocket(clientsocket);
    clientsocket=SOCK_DISCONNECTED;
    MsgBox("Could not connect to server %s:%i.",tbuf,port);
    EnableWindow(hwndListen, TRUE);
    EnableWindow(hwndConnect, TRUE);
    ExitThread(0);
  }

  datasize=MAX_SOCKET_BUFFER_SIZE;
  i=sizeof(datasize);
  setsockopt(clientsocket, SOL_SOCKET, SO_RCVBUF, (char *)&datasize, i); // how much data to buffer before we halt the server from sending any more (can't decode it fast enough)
  i=sizeof(datasize);
  getsockopt(clientsocket, SOL_SOCKET, SO_RCVBUF, (char *)&datasize, &i);
  if (datasize!=MAX_SOCKET_BUFFER_SIZE) { DBGINT;
    MAlert("Could not set desired outbut socket buffer size (currently %i). Performance may be degraded.",datasize);
  }
  datasize=-1;

  i=1;
  setsockopt(clientsocket, IPPROTO_TCP, TCP_NODELAY, (char *)&i, sizeof(int));

  if (encryption) { DBGINT;
    SetWindowText(hwndConnect, "Authenticating");

    crypt_state=CRYPT_PENDING;
    MEMORY_BARRIER; // don't want the order of these variables getting moved around
    crypt_mode=CRYPT_AUTH;
    MEMORY_BARRIER; // don't want the order of these variables getting moved around
    WakeThread(crypt_thread);
    while ((*(volatile int *)&crypt_state)==CRYPT_PENDING)
      Sleep(100);
    MEMORY_BARRIER; // read-acquire

    if (crypt_state==CRYPT_FAIL) { DBGINT;
      memset(passphrase, 0, passphrase_length);
      passphrase_set=FALSE;
      closesocket(clientsocket);
      clientsocket=SOCK_DISCONNECTED;
      MsgBox("Authentication failed.");
      SetWindowText(hwndConnect, "Connect");
      EnableWindow(hwndListen, TRUE);
      EnableWindow(hwndConnect, TRUE);
      ExitThread(0);
    }
  }

  clientActive=TRUE;

  start_x264_stream_display();
  start_CELT_stream_output();

  abuffer=cb_create(ARECV_BUFFER_SIZE,0);
  vbuffer=cb_create(VRECV_BUFFER_SIZE,16);
  videoWorker=CreateThread(NULL, 0, workerThread, (LPVOID)2, 0, NULL);
  audioWorker=CreateThread(NULL, 0, workerThread, (LPVOID)2, 0, NULL);
  lowPriorityDataWorker=CreateThread(NULL, 0, lowPriorityDataThread, NULL, 0, NULL);

  SetWindowText(hwndConnect, "Disconnect");
  EnableWindow(hwndConnect, TRUE);

  SetWindowLong(mainWindow, GWL_STYLE, MAINWINDOWSTYLE_PLAYER);

  ShowWindow(hwndIP,SW_HIDE);
  ShowWindow(hwndConnect,SW_HIDE);
  ShowWindow(hwndListen,SW_HIDE);
  ShowWindow(hwndText,SW_HIDE);
  ShowWindow(hwndFPS,SW_HIDE);
  ShowWindow(hwndCRF,SW_HIDE);
  ShowWindow(hwndBITRATE,SW_HIDE);
  ShowWindow(hwndBITRATEL,SW_HIDE);
  ShowWindow(hwndABITRATE,SW_HIDE);
  ShowWindow(hwndABITRATEL,SW_HIDE);
  ShowWindow(hwndACB,SW_HIDE);
  ShowWindow(hwndACBL,SW_HIDE);
  ShowWindow(hwndAC,SW_HIDE);
  ShowWindow(hwndACL,SW_HIDE);
  ShowWindow(hwndFPSL,SW_HIDE);
  ShowWindow(hwndCRFL,SW_HIDE);
  ShowWindow(hwndSCALE,SW_HIDE);
  ShowWindow(hwndSCALEL,SW_HIDE);
  ShowWindow(hwndAudioDevice,SW_HIDE);
  ShowWindow(hwndAudioDeviceL,SW_HIDE);
  ShowWindow(hwndABL,SW_HIDE);
  ShowWindow(hwndABM,SW_HIDE);
  ShowWindow(hwndABLL1,SW_HIDE);
  ShowWindow(hwndABLL2,SW_HIDE);
  ShowWindow(hwndFD,SW_HIDE);
  ShowWindow(hwndSPEED,SW_HIDE);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC1),SW_HIDE);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC2),SW_HIDE);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC3),SW_HIDE);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC4),SW_HIDE);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC5),SW_HIDE);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC6),SW_HIDE);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC7),SW_HIDE);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC8),SW_HIDE);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC9),SW_HIDE);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC10),SW_HIDE);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC11),SW_HIDE);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC12),SW_HIDE);

  menu=GetSystemMenu(mainWindow,FALSE);
  InsertMenu(menu,0,MF_BYPOSITION | MF_SEPARATOR,1235,NULL);
  InsertMenu(menu,0,MF_BYPOSITION | MF_ENABLED | MF_STRING,MENU_DISCONNECT,"Disconnect");

  FD_ZERO(&r);
  FD_SET(clientsocket, &r);
  timeout.tv_sec=1;
  timeout.tv_usec=0;

  bStarted=FALSE;
  ResetEvent(socketEvent);
  while (clientActive && select(0, &r, NULL, NULL, &timeout)!=SOCKET_ERROR) { DBGINT;
    SleepEx(0,TRUE);

    if (FD_ISSET(clientsocket, &r)) { DBGINT; // read client data
      i=recv_interrupted(clientsocket, (char *)&datasize, sizeof(int), 0);
      if (i!=0 && i!=SOCKET_ERROR && i==sizeof(int) && encryption)
        decrypt(&datasize,sizeof(int));
      if (i==0 || i==SOCKET_ERROR || i!=sizeof(int) || datasize<1) { DBGINT;
        if (i!=SOCKET_ERROR && i!=0) { DBGINT;
          if (i!=4) { DBGINT;
            MAlert("Received data int of size %i. Disconnected.",i);
          }
          else if (datasize<1) { DBGINT;
            MAlert("Received data int of value %i. Disconnected.",datasize);
          }
        }
        clientActive=FALSE;
        break;
      }

      i=recv_interrupted(clientsocket, (char *)&recvType, sizeof(recvType), 0);
      if (i==0 || i==SOCKET_ERROR) { DBGINT;
        clientActive=FALSE;
        break;
      }
      if (encryption)
        decrypt(&recvType,sizeof(recvType));

      switch (recvType) { DBGINT;
        case TRANSMIT_VIDEO:
          if (datasize+FF_INPUT_BUFFER_PADDING_SIZE>VRECV_BUFFER_SIZE) { DBGINT;
            MAlert("Received video data int of value %i (max %i; would cause receive buffer overflow). Disconnected.",datasize,VRECV_BUFFER_SIZE-FF_INPUT_BUFFER_PADDING_SIZE);
            clientActive=FALSE;
            break;
          }
          while ((*(volatile int *)&oframe)>30) {
            if (!tcpThrottling) {
              tcpThrottling=1500;
              cmd.cmd=CMD_SETTCPWINDOW;
              cmd.param1=CMD_SETTCPWINDOW_SET;
              cmd.param2=1500;
              transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);
            }
            Sleep(1);
          }
          if (tcpThrottling && (*(volatile int *)&oframe)<15) {
            tcpThrottling=0;
            cmd.cmd=CMD_SETTCPWINDOW;
            cmd.param1=CMD_SETTCPWINDOW_UNSET;
            cmd.param2=0;
            transmit(clientsocket,&cmd,sizeof(cmd),TRANSMIT_CMD);
          }
          benchmark_range(alabel,BM_START,&a);
          while ((i=(ret=cb_recv_pad(clientsocket, vbuffer, datasize, 0, FF_INPUT_BUFFER_PADDING_SIZE, 0)).size)<1) { DBGINT;
            if (i==CB_OVERFLOW) { DBGINT;
              benchmark_range(alabel,BM_STOP,&a);
              if (!warned) { DBGINT; warned=TRUE; MAlert("Video receive buffer overflow; your client machine may not be powerful enough to decode at these settings and this would result in low framerate / picture delay."); }
              Sleep(1);
              benchmark_range(alabel,BM_START,&a);
            }
            if (i==0 || i==SOCKET_ERROR) { DBGINT;
              clientActive=FALSE;
              break;
            }
          }
          benchmark_range(alabel,BM_STOP,&a);

          if (clientActive==FALSE) break;

          benchmark_range(blabel,BM_START,&a);
          if (encryption)
            decrypt(ret.data,datasize);
          benchmark_range(blabel,BM_STOP,&a);
          EnterCriticalSection(&oframelock);
          oframe++;
          LeaveCriticalSection(&oframelock);
          QueueUserAPC(UpdateMainWindow, videoWorker, (ULONG_PTR)datasize);
          break;
        case TRANSMIT_AUDIO:
          if (datasize>ARECV_BUFFER_SIZE) { DBGINT;
            MAlert("Received audio data int of value %i (max %i; would cause receive buffer overflow). Disconnected.",datasize,ARECV_BUFFER_SIZE);
            clientActive=FALSE;
            break;
          }

          benchmark_range(alabel,BM_START,&a);
          while ((i=(ret=cb_recv(clientsocket, abuffer, datasize, 0)).size)<1) { DBGINT;
            if (i==CB_OVERFLOW) { DBGINT;
              benchmark_range(alabel,BM_STOP,&a);
              if (!warned) { DBGINT; warned=TRUE; MAlert("Audio receive buffer overflow; your client machine may not be powerful enough to decode at these settings."); }
              Sleep(1);
              benchmark_range(alabel,BM_START,&a);
            }
            if (i==0 || i==SOCKET_ERROR) { DBGINT;
              clientActive=FALSE;
              break;
            }
          }
          benchmark_range(alabel,BM_STOP,&a);

          if (clientActive==FALSE) break;

          benchmark_range(blabel,BM_START,&a);
          if (encryption)
            decrypt(ret.data,datasize);
          benchmark_range(blabel,BM_STOP,&a);

          QueueUserAPC(play_audio_buffer, audioWorker, (ULONG_PTR)datasize);
          break;
        case TRANSMIT_CMD:
        case TRANSMIT_CURSOR:
          if (datasize>RECV_BUFFER_SIZE) { DBGINT;
            MAlert("Received command data int of value %i (max %i; would cause receive buffer overflow). Disconnected.",datasize,RECV_BUFFER_SIZE);
            clientActive=FALSE;
            break;
          }
          benchmark_range(alabel,BM_START,&a);
          i=recv_interrupted(clientsocket, recvbuffer, datasize, 0);
          if (i==0 || i==SOCKET_ERROR) { DBGINT;
            clientActive=FALSE;
            break;

          }
          benchmark_range(alabel,BM_STOP,&a);
          benchmark_range(blabel,BM_START,&a);
          if (encryption)
            decrypt(recvbuffer,datasize);
          benchmark_range(blabel,BM_STOP,&a);
          switch (recvType) { DBGINT;
            case TRANSMIT_CMD:
              process_command((cmdStruct *)recvbuffer,datasize);
              break;
            case TRANSMIT_CURSOR:
              setCursor((cursorData *)recvbuffer,datasize);
              break;
          }
          break;
      }
    }
    FD_ZERO(&r); // set sockets to poll
    FD_SET(clientsocket, &r);
  }

  closesocket(clientsocket);
  clientsocket=SOCK_DISCONNECTED;
  SetEvent(socketEvent);
  clientActive=FALSE;

  datasize=i=j=0;
  QueueUserAPC(exitWorkerAPC, videoWorker, (ULONG_PTR)&datasize);
  QueueUserAPC(exitWorkerAPC, audioWorker, (ULONG_PTR)&i);
  QueueUserAPC(exitWorkerAPC, lowPriorityDataWorker, (ULONG_PTR)&j);
  while ((*(volatile int *)&datasize)==0 || (*(volatile int *)&i)==0 || (*(volatile int *)&j)==0) Sleep(0); // wait for worker threads to quit
  MEMORY_BARRIER;

  cb_free(abuffer);
  cb_free(vbuffer);
  vbuffer=NULL;

  release_audio_variables();
  release_allocated_variables();
  release_transfer_variables();

  if (fcsEngaged) { DBGINT;
    LeaveCriticalSection(&fmtstartlock);
    fcsEngaged=FALSE;
  }

  deleteFileList(&file_list);

  GetSystemMenu(mainWindow,TRUE);

  AttachThreadInput(GetCurrentThreadId(),windowThreadId,TRUE);

  ShowWindow(hwndFPSL,SW_SHOW);
  ShowWindow(hwndCRFL,SW_SHOW);
  ShowWindow(hwndBITRATEL,SW_SHOW);
  ShowWindow(hwndBITRATE,SW_SHOW);
  ShowWindow(hwndABITRATEL,SW_SHOW);
  ShowWindow(hwndABITRATE,SW_SHOW);
  ShowWindow(hwndFPS,SW_SHOW);
  ShowWindow(hwndCRF,SW_SHOW);
  ShowWindow(hwndIP,SW_SHOW);
  ShowWindow(hwndConnect,SW_SHOW);
  ShowWindow(hwndListen,SW_SHOW);
  ShowWindow(hwndText,SW_SHOW);
  ShowWindow(hwndACB,SW_SHOW);
  ShowWindow(hwndACBL,SW_SHOW);
  ShowWindow(hwndAC,SW_SHOW);
  ShowWindow(hwndACL,SW_SHOW);
  ShowWindow(hwndSCALE,SW_SHOW);
  ShowWindow(hwndSCALEL,SW_SHOW);
  ShowWindow(hwndAudioDevice,SW_SHOW);
  ShowWindow(hwndAudioDeviceL,SW_SHOW);
  ShowWindow(hwndABL,SW_SHOW);
  ShowWindow(hwndABM,SW_SHOW);
  ShowWindow(hwndABLL1,SW_SHOW);
  ShowWindow(hwndABLL2,SW_SHOW);
  ShowWindow(hwndFD,SW_SHOW);
  ShowWindow(hwndSPEED,SW_SHOW);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC1),SW_SHOW);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC2),SW_SHOW);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC3),SW_SHOW);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC4),SW_SHOW);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC5),SW_SHOW);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC6),SW_SHOW);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC7),SW_SHOW);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC8),SW_SHOW);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC9),SW_SHOW);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC10),SW_SHOW);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC11),SW_SHOW);
  ShowWindow(GetDlgItem(mainWindow,IDC_STATIC12),SW_SHOW);
  resetCursor();

  SetFocus(hwndConnect);

  SetWindowText(hwndConnect, "Connect");
  EnableWindow(hwndListen, TRUE);
  EnableWindow(hwndConnect, TRUE);
  InvalidateRgn(mainWindow, NULL, FALSE);
  UpdateWindow(mainWindow);
  resetMainWindow();
  CloseHandle(client_thread);
  client_thread=NULL;
  ExitThread(1);
}

BOOL NODELAY=TRUE;
int transmit(SOCKET s, void *ptr, int len, BYTE transmitType) { DBGINT;
  return transmit_data(s, ptr, len, transmitType, TRUE, FALSE);
}
int transmit_bulk(SOCKET s, void *ptr, int len, BYTE transmitType) { DBGINT;
  return transmit_data(s, ptr, len, transmitType, FALSE, FALSE);
}
int transmit_bulk_insitu(SOCKET s, int len, BYTE transmitType) { DBGINT;
  return transmit_data(s, NULL, len, transmitType, FALSE, TRUE);
}
int transmit_data(SOCKET s, void *ptr, int len, BYTE transmitType, BOOL nodelay, BOOL insitu) { DBGINT;
  int i;
  transmit_buffer *t;

  if (!insitu)
    EnterCriticalSection(&sendlock);
  if (!transbuf)
    transbuf=malloc(sizeof(transmit_buffer));

  if (sizeof(transmit_buffer) < len+sizeof(transmitType)+sizeof(len)) // try to avoid repeated calls to malloc
    t=malloc(len+sizeof(len)+sizeof(transmitType));
  else
    t=transbuf;

  t->len=len;
  t->transmitType=transmitType;

  if (transmitType==TRANSMIT_CMD && ((cmdStruct*)(t->data))->cmd==CMD_CLIPBOARD)
    totalVBytes+=len+sizeof(transmitType)+sizeof(len);

  if (!insitu)
    memcpy(t->data,ptr,len);
  if (encryption)
    encrypt(t,len+sizeof(int)+sizeof(transmitType));

/* begin debugging code - throttles bandwidth

int debug_iterations,debug_i,debug_remainder,debug_len;
if (max_bitrate>1000) { DBGINT;
  i=send(s,(const char *)t,sizeof(transmitType)+sizeof(len),0);
  debug_iterations=(1000/100.0)*len/(50*1000); // number of iterations at 50kbps
  if (debug_iterations==0) debug_iterations=1;
  debug_remainder=len-debug_iterations*(len/debug_iterations);
  debug_len=(len-debug_remainder)/debug_iterations;
  for (debug_i=0; debug_i<debug_iterations; debug_i++) { DBGINT;
    if (i<=0) break;
    Sleep(100);
    i=send(s,(const char *)t+sizeof(transmitType)+sizeof(len)+debug_i*debug_len,debug_len,0);
    if (i<=0) break;
    if (debug_i+1==debug_iterations && debug_remainder>0)
      i=send(s,(const char *)t+sizeof(transmitType)+sizeof(len)+(debug_i+1)*debug_len,debug_remainder,0);
  }
  if (i>0) i=len;
}
else
*/

  if (nodelay != NODELAY) {
    NODELAY=nodelay;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&NODELAY, sizeof(BOOL));
  }

  i=send(s,(const char *)t,len+sizeof(transmitType)+sizeof(len),0);
  if (sizeof(transmit_buffer) < len+sizeof(transmitType)+sizeof(len))
    free(t);

  if (!insitu)
    LeaveCriticalSection(&sendlock);
  EnterCriticalSection(&byteslock);
  switch (transmitType) { DBGINT;
    case TRANSMIT_VIDEO:
      totalVBytes+=len+sizeof(transmitType)+sizeof(len); break;
    case TRANSMIT_AUDIO:
      totalABytes+=len+sizeof(transmitType)+sizeof(len); break;
  }
  totalSent+=len+sizeof(transmitType)+sizeof(len);
  LeaveCriticalSection(&byteslock);
  return i;
}

INT_PTR CALLBACK PasswordProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) 
{ 
    WORD cchPassword; 
    char pp[passphrase_length+6]={0};

    switch (message) 
    { 
        case WM_INITDIALOG: 
            SendDlgItemMessage(hDlg, IDC_PWD, EM_SETPASSWORDCHAR, (WPARAM) '*', (LPARAM) 0); 
            SendMessage(hDlg, DM_SETDEFID, (WPARAM) IDCANCEL, (LPARAM) 0); 
            SetFocus(GetDlgItem(hDlg, IDC_PWD));
            return FALSE;

        case WM_COMMAND: 
            // Set the default push button to "OK" when the user enters text. 
            if(HIWORD (wParam) == EN_CHANGE && 
                                LOWORD(wParam) == IDC_PWD) 
            {
                SendMessage(hDlg, DM_SETDEFID, (WPARAM) IDOK, (LPARAM) 0); 
            }
            switch(wParam) 
            { 
                case IDOK: 
                    // Get number of characters. 
                    cchPassword = (WORD) SendDlgItemMessage(hDlg, IDC_PWD, EM_LINELENGTH, (WPARAM) 0, (LPARAM) 0); 
                    if (cchPassword < 6) 
                    { 
                        MessageBox(NULL, "Too few characters entered (minimum of 6).", "Error", MB_OK); 
                        break;
                    } 

                    if (cchPassword > passphrase_length) 
                    { 
                        MessageBox(NULL, "Too many characters entered (maximum of 64).", "Error", MB_OK); 
                        break; 
                    } 

                    GetDlgItemText(hDlg, IDC_PWD, pp, passphrase_length+1);

                    strcat(pp,"ABCDE"); // salt

                    if (gcry_md_get_algo_dlen(PASSPHRASE_DIGEST_ALGO)!=passphrase_length) {
                        MessageBox(NULL, "Message digest length and passphrase_length don't match.", "Error", MB_OK); 
                        SafeShutdown();
                    }

                    gcry_md_hash_buffer(PASSPHRASE_DIGEST_ALGO,passphrase,pp,cchPassword+5);

                    memset(pp, 'A', passphrase_length+4);
                    pp[passphrase_length-1]=0;
                    SetDlgItemText(hDlg, IDC_PWD, pp);
                    passphrase_set=TRUE;
                    EndDialog(hDlg, TRUE); 
                    break; 

                case IDCANCEL: 
                    EndDialog(hDlg, TRUE); 
                    break; 
            } 
            break; 
      default:
        return FALSE;
    } 
    return TRUE;
}

void start_server(void) { DBGINT;
  if (!passphrase_set)
    DialogBox(hInst, MAKEINTRESOURCE(IDD_PASSWORD), mainWindow, PasswordProc);
  if (!passphrase_set) { DBGINT;
    EnableWindow(hwndListen, TRUE);
    EnableWindow(hwndConnect, TRUE);
    EnableWindow(hwndIP, TRUE);
    return;
  }
  CloseHandle(CreateThread(NULL, 0, serverloop, NULL, 0, NULL));
}

cursorData *getCursor(HCURSOR iCursor);

void testCursor(void) { DBGINT;
  HCURSOR testcursor;
  cursorData *mc;
  testcursor=LoadCursor(NULL,MAKEINTRESOURCE(32648));
  mc=getCursor(testcursor);
  setCursor(mc,sizeof(cursorData)+mc->width*mc->height*mc->bpp/4);
}

void start_client(void) { DBGINT;
  if (!passphrase_set)
    DialogBox(hInst, MAKEINTRESOURCE(IDD_PASSWORD), mainWindow, PasswordProc);
  if (!passphrase_set) { DBGINT;
    EnableWindow(hwndListen, TRUE);
    EnableWindow(hwndConnect, TRUE);
    return;
  }
  client_thread=CreateThread(NULL, 0, clientloop, NULL, 0, NULL);
}

HCURSOR lastCursor=0;
HCURSOR *sentCursors=NULL;
cursorData **gotCursors=NULL;
int sentCursorsLen=0,gotCursorsLen=0;

int compare_hcursors (const void *a, const void *b) { DBGINT;
  return *(int *)a-*(int *)b;
}
int compare_cursordata (const void *a, const void *b) { DBGINT;
  return ((int)((cursorData *)*(cursorData **)a)->handle) - ((int)((cursorData *)*(cursorData **)b)->handle);
}

int bsearch_compare_cursordata (const void *a, const void *b) { DBGINT;
  return ((int)((cursorData *)a)->handle) - ((int)(*(cursorData **)b)->handle);
}

void resetCursor(void) { DBGINT;
  HCURSOR ocursor;
  int i;
  if (sentCursors)
    free(sentCursors);
  for (i=0; i<gotCursorsLen; i++)
    if (gotCursors[i]!=NULL)
      free(gotCursors[i]);
  if (gotCursors)
    free(gotCursors);
  gotCursors=NULL;
  sentCursors=NULL;
  gotCursorsLen=sentCursorsLen=0;
  lastCursor=0;

  ocursor=(HCURSOR)GetClassLong(mainWindow, GCL_HCURSOR);

  if (ocursor!=defaultCursor) { DBGINT;
    SetClassLong(mainWindow, GCL_HCURSOR, (LONG)defaultCursor);
    if (ocursor!=NULL)
      DestroyCursor(ocursor);
  }
}

void checkCursor(void) { DBGINT;
  cursorData *mc;
  CURSORINFO cursorInfo = { 0 };

  cursorInfo.cbSize = sizeof(cursorInfo);

  if (!GetCursorInfo(&cursorInfo)) return;
  if (lastCursor==cursorInfo.hCursor) return;
  lastCursor=cursorInfo.hCursor;

  if (!lastCursor || cursorInfo.flags==0) { DBGINT;
    mc=NULL;
    transmit(clientsocket,&mc,sizeof(HCURSOR),TRANSMIT_CURSOR);
    return;
  }

  if (bsearch(&lastCursor, sentCursors, sentCursorsLen, sizeof(HCURSOR), compare_hcursors)) { DBGINT;
    transmit(clientsocket,&lastCursor,sizeof(HCURSOR),TRANSMIT_CURSOR);
  } else {
    // send hcursor
    sentCursors=realloc(sentCursors,sizeof(HCURSOR)*(++sentCursorsLen));
    sentCursors[sentCursorsLen-1]=lastCursor;
    qsort(sentCursors, sentCursorsLen, sizeof(HCURSOR), compare_hcursors);
    if (mc=getCursor(cursorInfo.hCursor)) { DBGINT;
      transmit(clientsocket,mc,sizeof(cursorData)+(mc->width*mc->height*(mc->bpp==1 ? 2 : mc->bpp+1)/8),TRANSMIT_CURSOR);
      free(mc);
    }
  }
}

void setCursor(cursorData *dcursor, int size) { DBGINT;
  HCURSOR tcursor,ocursor;
  HBITMAP ORbmp,ANDbmp;
  BITMAP tbmp;
  ICONINFO ii;
  cursorData tc,*mcursor=NULL;
  POINT pcursor;
  BYTE CursorMaskAND[] = { 0xFF };
  BYTE CursorMaskXOR[] = { 0x00 };
  int i;

  char tmpbuf[1000];

  if (size<1) return;
  if (size==sizeof(HCURSOR)) { DBGINT;
    if (*(HCURSOR*)dcursor==0) { DBGINT;
      tcursor=CreateCursor(hInst,0,0,1,1,CursorMaskAND,CursorMaskXOR);
      ocursor=(HCURSOR)GetClassLong(mainWindow, GCL_HCURSOR);

      SetClassLong(mainWindow, GCL_HCURSOR, (LONG)tcursor);
      GetCursorPos(&pcursor);
      captureMouseMove=FALSE;
      SetCursorPos(pcursor.x,pcursor.y);

      if (ocursor!=NULL)
        DestroyCursor(ocursor);
      return;
    }
    else {
      tc.handle=*(HCURSOR*)dcursor;
      if (!(mcursor=bsearch(&tc, gotCursors, gotCursorsLen, sizeof(cursorData *), bsearch_compare_cursordata))) { DBGINT;
        Debug("ERROR: Lost cursor! %i", (int)*(HCURSOR*)dcursor);
        return;
      }
      mcursor=*(cursorData **)mcursor;
    }
  } else {
    mcursor=malloc(size);
    memcpy(mcursor,dcursor,size);
    gotCursors=realloc(gotCursors,sizeof(cursorData *)*(++gotCursorsLen));
    gotCursors[gotCursorsLen-1]=mcursor;
    qsort(gotCursors, gotCursorsLen, sizeof(cursorData *), compare_cursordata);
  }

  if (mcursor->bpp==1)
    tcursor=CreateCursor(hInst,mcursor->xHS,mcursor->yHS,mcursor->width,mcursor->height,((BYTE *)mcursor)+sizeof(cursorData),((BYTE *)mcursor)+sizeof(cursorData)+mcursor->width*mcursor->height*mcursor->bpp/8);
  else {
    ANDbmp=CreateBitmap(mcursor->width,mcursor->height,1,1,((BYTE *)mcursor)+sizeof(cursorData));
    ORbmp=CreateBitmap(mcursor->width,mcursor->height,1,mcursor->bpp,((BYTE *)mcursor)+sizeof(cursorData)+mcursor->width*mcursor->height/8);
    ii.fIcon=FALSE;
    ii.xHotspot=mcursor->xHS;
    ii.yHotspot=mcursor->yHS;
    ii.hbmMask=ANDbmp;
    ii.hbmColor=ORbmp;
    tcursor=CreateIconIndirect(&ii);
    DeleteObject(ANDbmp);
    DeleteObject(ORbmp);
  }
  ocursor=(HCURSOR)GetClassLong(mainWindow, GCL_HCURSOR);

  SetClassLong(mainWindow, GCL_HCURSOR, (LONG)tcursor);
  GetCursorPos(&pcursor);
  captureMouseMove=FALSE;
  SetCursorPos(pcursor.x,pcursor.y);
  if (ocursor!=NULL) { DBGINT;
    if (!GetIconInfo(ocursor, &ii)) return;
    if (!GetObject(ii.hbmMask, sizeof(tbmp), &tbmp)) return;
    if (tbmp.bmBitsPixel==1)
      DestroyCursor(ocursor);
    else
      DestroyIcon(ocursor);
  }
}

cursorData *getCursor(HCURSOR iCursor) { DBGINT;
  ICONINFO ii = {0};
  HDC memDC;
  cursorData *mcursor=NULL;  
  BITMAPINFO bmi;
  BITMAP tbmp;
  int i;
  BYTE *tbuffer;
  BYTE *ORbmp;
  BYTE *ANDbmp;

//  HCURSOR tcursor;
//  HCURSOR testcursor;

  if (!GetIconInfo(iCursor, &ii)) return NULL;

//testcursor=LoadCursor(NULL,MAKEINTRESOURCE(32648));
//if (!GetIconInfo(testcursor, &ii)) return NULL;

  memDC = CreateCompatibleDC(hDCScreen);

  tbmp.bmType=0;
  if (!GetObject(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof(tbmp), &tbmp)) return NULL;

  mcursor=malloc(sizeof(cursorData)+tbmp.bmWidth*tbmp.bmHeight*((ii.hbmColor ? tbmp.bmBitsPixel : 0)+1)/8);

  mcursor->xHS=ii.xHotspot;
  mcursor->yHS=ii.yHotspot;
  mcursor->width=tbmp.bmWidth;
  mcursor->height=tbmp.bmHeight;
  mcursor->bpp=tbmp.bmBitsPixel;
  mcursor->handle=iCursor;

  memset(&bmi, 0, sizeof(BITMAPINFO)); 
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth=mcursor->width;
  bmi.bmiHeader.biHeight=mcursor->height;
  bmi.bmiHeader.biPlanes=1;
  bmi.bmiHeader.biBitCount=1;
  bmi.bmiHeader.biCompression=BI_RGB;

  ANDbmp=(BYTE *)mcursor+sizeof(cursorData);
  ORbmp=(BYTE *)mcursor+sizeof(cursorData)+tbmp.bmWidth*tbmp.bmHeight*1/(ii.hbmColor ? 8 : 16);

  tbuffer=malloc(mcursor->height*mcursor->width*tbmp.bmBitsPixel/8);
  if (mcursor->bpp!=1) { DBGINT;
    GetDIBits(memDC, ii.hbmMask, 0, mcursor->height, tbuffer, &bmi, DIB_RGB_COLORS);
    // flip hbmMask bits
    for (i=0; i<mcursor->height; i++)
      memcpy(ANDbmp+(mcursor->height-1-i)*mcursor->width*1/8, tbuffer+i*mcursor->width*1/8, mcursor->width*1/8);

    bmi.bmiHeader.biBitCount=tbmp.bmBitsPixel;
    GetDIBits(memDC, ii.hbmColor, 0, mcursor->height, tbuffer, &bmi, DIB_RGB_COLORS);
    // flip hbmColor bits
    for (i=0; i<mcursor->height; i++)
      memcpy(ORbmp+(mcursor->height-1-i)*mcursor->width*tbmp.bmBitsPixel/8, tbuffer+i*mcursor->width*tbmp.bmBitsPixel/8, mcursor->width*tbmp.bmBitsPixel/8);
  }
  else {
    GetDIBits(memDC, ii.hbmMask, 0, mcursor->height, tbuffer, &bmi, DIB_RGB_COLORS);
    // flip hbmMask bits and split them up into ANDbmp and ORbmp
    for (i=0; i<mcursor->height/2; i++) // OR plane
      memcpy(ORbmp+(mcursor->height/2-1-i)*mcursor->width*tbmp.bmBitsPixel/8, tbuffer+i*mcursor->width*tbmp.bmBitsPixel/8, mcursor->width*tbmp.bmBitsPixel/8);

    for (i=mcursor->height/2; i<mcursor->height; i++) // AND plane
      memcpy(ANDbmp+(mcursor->height-1-i)*mcursor->width*tbmp.bmBitsPixel/8, tbuffer+i*mcursor->width*tbmp.bmBitsPixel/8, mcursor->width*tbmp.bmBitsPixel/8);
    mcursor->height=tbmp.bmHeight/2;
  }

//  tcursor=CreateCursor(hInst,mcursor->xHS,mcursor->yHS,mcursor->width,mcursor->height,((BYTE *)mcursor)+sizeof(cursorData),((BYTE *)mcursor)+sizeof(cursorData)+(mcursor->width)*(mcursor->height)*(mcursor->bpp)/8);
//  tcursor=CreateCursor(hInst,mcursor->xHS,mcursor->yHS,mcursor->width,mcursor->height,ANDbmp,ORbmp);
//  SetClassLong(mainWindow, GCL_HCURSOR, (LONG)tcursor);

  free(tbuffer);
  DeleteObject(ii.hbmMask);
  DeleteObject(ii.hbmColor);
  DeleteDC(memDC);

  return mcursor;
}

/*
#include <imagehlp.h>
void __cyg_profile_func_enter (void *this_fn, void *call_site) { DBGINT;
  printf("S: 0x%p\n", (int *)this_fn);
}
void __cyg_profile_func_exit (void *this_fn, void *call_site) { DBGINT;
  printf("E: 0x%p\n", (int *)this_fn);
}

void __cyg_profile_func_enter (void *this_fn, void *call_site) { DBGINT;
  static int *initial=NULL, *x=NULL;
  DWORD  error;
  static HANDLE hProcess;
  static char symbuffer[sizeof(IMAGEHLP_SYMBOL) + 100 * sizeof(TCHAR)];
  static PIMAGEHLP_SYMBOL pSymbol;

  if (!initial) { DBGINT;
    initial=this_fn;
    x=call_site;

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    hProcess = GetCurrentProcess();

    if (!SymInitialize(hProcess, NULL, TRUE)) { DBGINT;
      error = GetLastError();
      Alert("SymInitialize returned error : %d\n", error);
      return;
    }
    pSymbol = (PIMAGEHLP_SYMBOL)symbuffer;
    pSymbol->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL);
    pSymbol->MaxNameLength = 100;
  }


  if (SymGetSymFromAddr(hProcess, (DWORD)call_site, NULL, pSymbol)) { DBGINT;
    printf("%s -> ", pSymbol->Name);
  }
  if (SymGetSymFromAddr(hProcess, (DWORD)this_fn, NULL, pSymbol)) { DBGINT;
    printf("%s", pSymbol->Name);
  }
  printf("\n");
//  else
//    printf("%p -> %p\n", call_site, this_fn);
}
*/