// $Id: monitor.c,v 1.19 2017/09/02 05:48:30 karn Exp karn $
// Listen to multicast, send PCM audio to Linux ALSA driver
#define _GNU_SOURCE 1
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <string.h>
#include <opus/opus.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <netdb.h>
#include <portaudio.h>
#include <ncurses.h>
#include <locale.h>

#include "rtp.h"
#include "dsp.h"
#include "multicast.h"

struct audio {
  struct audio *prev;       // Linked list pointers
  struct audio *next; 
  uint32_t ssrc;            // RTP Sending Source ID
  int eseq;                 // Next expected RTP sequence number
  int etime;                // Next expected RTP timestamp
  int type;                 // RTP type (10,11,20)
  
  struct sockaddr sender;
  char addr[NI_MAXHOST];    // RTP Sender IP address
  char port[NI_MAXSERV];    // RTP Sender source port
  OpusDecoder *opus;        // Opus codec decoder handle, if needed
  int opus_frame_size;      // Opus frame size in samples
  int channels;             // Channels (1 or 2)
  int opus_bandwidth;       // Opus stream audio bandwidth
  PaStream *Pa_Stream;      // Portaudio stream handle
#define AUD_BUFSIZE 65536   // 1.37 sec at 48000 Hz
  int16_t audiobuffer[AUD_BUFSIZE]; // Audio playout buffer
  volatile int write_ptr;            // Playout buffer write pointer
  volatile int read_ptr;             //                read_pointer, for callback
  unsigned long packets;    // RTP packets for this session
  unsigned long drops;      // Apparent rtp packet drops
  int age;                  // Display cycles since last active
  int idle;                 // Device overrun on last callback implies idle
  unsigned long underruns;  // Callback count of underruns (stereo samples) replaced with silence
  PaTime hw_delay;          // Estimated playout delay, calculated in callback
  PaTime last_written_time; // Time of last write to playout buffer, for delay calculations
};

// Global variables
char *Mcast_address_text;     // Multicast address we're listening to
const int Bufsize = 8192;     // Maximum samples/words per RTP packet - must be bigger than Ethernet MTU
const int Samprate = 48000;   // Too hard to handle other sample rates right now
int Verbose;                  // Verbosity flag (currently unused)
#define HASHCHAINS 16         // Make this a power of 2 for efficiency
struct audio *Audio[HASHCHAINS];     // Hash chains for session structures
int Input_fd = -1;            // Multicast receive socket
int inDevNum;                 // Portaudio's audio output device index
char Audiodev[256];           // Name of audio device; empty means portaudio's default
pthread_t Display_thread;     // Display thread descriptor
int Update_interval = 100000; // Time in usec between display updates
int Inactive_interval = 1;    // Time in sec until inactive entry un-bolds
int Clear_interval = 20;      // Time in sec until inactive entry is cleared

// The Audio structures are accessed by both display() and main(), so protect them
pthread_mutex_t Audio_mutex;

void closedown(int);
void *display(void *);
struct audio *lookup_session(const struct sockaddr *,uint32_t);
struct audio *make_session(struct sockaddr const *r,uint32_t,uint16_t,uint32_t);
int close_session(struct audio *);
static int pa_callback(const void *,void *,unsigned long,const PaStreamCallbackTimeInfo*,PaStreamCallbackFlags,void *);
static void pa_complete(void *);


int main(int argc,char * const argv[]){
  // Try to improve our priority
  int prio = getpriority(PRIO_PROCESS,0);
  prio = setpriority(PRIO_PROCESS,0,prio - 5);

  // Drop root if we have it
  seteuid(getuid());

  setlocale(LC_ALL,getenv("LANG"));
  Mcast_address_text = "239.2.1.1";
  int c;
  while((c = getopt(argc,argv,"S:I:v")) != EOF){
    switch(c){
    case 'a':
      strncpy(Audiodev,optarg,sizeof(Audiodev));
      break;
    case 'v':
      Verbose++;
      break;
    case 'I':
      Mcast_address_text = optarg;
      break;
    default:
      fprintf(stderr,"Usage: %s [-v] [-I mcast_address]\n",argv[0]);
      fprintf(stderr,"Defaults: %s -I %s\n",argv[0],Mcast_address_text);
      exit(1);
    }
  }

  PaError r = Pa_Initialize();
  if(r != paNoError){
    fprintf(stderr,"Portaudio error: %s\n",Pa_GetErrorText(r));
    return r;
  }

  if(strlen(Audiodev) == 0){
    // not specified; use default
    inDevNum = Pa_GetDefaultOutputDevice();
  } else {
    // Find requested audio device in the list
    int numDevices = Pa_GetDeviceCount();
    
    for(inDevNum=0; inDevNum < numDevices; inDevNum++){
      const PaDeviceInfo *deviceInfo = Pa_GetDeviceInfo(inDevNum);
      if(strcmp(deviceInfo->name,Audiodev) == 0)
	break;
    }
  }
  if(inDevNum == paNoDevice){
    fprintf(stderr,"Portaudio: no available devices\n");
    return -1;
  }
  // Set up multicast input
  Input_fd = setup_mcast(Mcast_address_text,0);
  if(Input_fd == -1){
    fprintf(stderr,"Can't set up input\n");
    exit(1);
  }
  struct iovec iovec[2];
  struct rtp_header rtp;
  int16_t data[Bufsize];
  
  iovec[0].iov_base = &rtp;
  iovec[0].iov_len = sizeof(rtp);
  iovec[1].iov_base = data;
  iovec[1].iov_len = sizeof(data);

  struct msghdr message;
  struct sockaddr sender;
  message.msg_name = &sender;
  message.msg_namelen = sizeof(sender);
  message.msg_iov = &iovec[0];
  message.msg_iovlen = 2;
  message.msg_control = NULL;
  message.msg_controllen = 0;
  message.msg_flags = 0;

  // Temporarily put socket in nonblocking state
  // and flush any pending packets to avoid unnecessary long buffer delays
  int flags;
  if((flags = fcntl(Input_fd,F_GETFL)) != -1){
    flags |= O_NONBLOCK;
    if(fcntl(Input_fd,F_SETFL,flags) == 0){
      int flushed = 0;
      while(recvmsg(Input_fd,&message,0) >= 0)
	flushed++;
      flags &= ~O_NONBLOCK;
      fcntl(Input_fd,F_SETFL,flags);
    }
  }

  // Graceful signal catch
  signal(SIGPIPE,closedown);
  signal(SIGINT,closedown);
  signal(SIGKILL,closedown);
  signal(SIGQUIT,closedown);
  signal(SIGTERM,closedown);
  signal(SIGPIPE,SIG_IGN);

  pthread_mutex_init(&Audio_mutex,NULL);
  pthread_create(&Display_thread,NULL,display,NULL);

  while(1){
    ssize_t size;

    if((size = recvmsg(Input_fd,&message,0)) < sizeof(rtp)){
      usleep(500); // Avoid tight loop
      continue; // Too small to be valid RTP
    }
    // To host order
    rtp.ssrc = ntohl(rtp.ssrc);
    rtp.seq = ntohs(rtp.seq);
    rtp.timestamp = ntohl(rtp.timestamp);

    pthread_mutex_lock(&Audio_mutex); // Keep display thread from modifying the table

    struct audio *sp = lookup_session(&sender,rtp.ssrc);
    if(sp == NULL){
      // Not found
      if((sp = make_session(&sender,rtp.ssrc,rtp.seq,rtp.timestamp)) == NULL){
	fprintf(stderr,"No room!!\n");
	goto endloop;
      }
      getnameinfo((struct sockaddr *)&sender,sizeof(sender),sp->addr,sizeof(sp->addr),
		    sp->port,sizeof(sp->port),NI_NOFQDN|NI_DGRAM|NI_NUMERICHOST);

    }
    sp->age = 0;
    int drop = 0;
    if(rtp.seq != sp->eseq){
      int const diff = (int)(rtp.seq - sp->eseq);
      //      fprintf(stderr,"ssrc %lx: expected %d got %d\n",(unsigned long)rtp.ssrc,sp->eseq,rtp.seq);
      if(diff < 0 && diff > -10)
	goto endloop;	// Drop probable duplicate
      drop = diff; // Apparent # packets dropped
      sp->drops += abs(drop);
    }
    sp->eseq = (rtp.seq + 1) & 0xffff;
    size -= sizeof(rtp); // Bytes in payload
    sp->packets++;

    int samples = 0;
    sp->type = rtp.mpt;
    switch(rtp.mpt){
    case 10: // Stereo
      sp->channels = 2;
      samples = size / 4;  // # 32-bit word samples
      for(int i=0;i<2*samples;i++){
	sp->audiobuffer[sp->write_ptr++] = ntohs(data[i]); // RTP profile specifies big-endian samples
	if(sp->write_ptr >= AUD_BUFSIZE)
	  sp->write_ptr -= AUD_BUFSIZE;
      }
      break;
    case 11: // Mono; send to both stereo channels
      sp->channels = 1;
      samples = size / 2;
      for(int i=0;i<samples;i++){
	assert(sp->write_ptr <= AUD_BUFSIZE -2);
	sp->audiobuffer[sp->write_ptr++] = ntohs(data[i]);
	sp->audiobuffer[sp->write_ptr++] = ntohs(data[i]);
	if(sp->write_ptr >= AUD_BUFSIZE)
	  sp->write_ptr -= AUD_BUFSIZE;
      }
      break;
    case 20: // Opus codec decode - arbitrary choice
      if(sp->opus == NULL){ // Create if it doesn't already exist
	int error;
	sp->opus = opus_decoder_create(Samprate,2,&error);
	if(sp->opus == NULL)
	  fprintf(stderr,"Opus decoder error %d\n",error);
	break;
      }
      sp->channels = opus_packet_get_nb_channels((unsigned char *)data);
      sp->opus_bandwidth = opus_packet_get_bandwidth((unsigned char *)data);
      int nb_frames = opus_packet_get_nb_frames((unsigned char *)data,size);
      sp->opus_frame_size = nb_frames * opus_packet_get_samples_per_frame((unsigned char *)data,Samprate);

      int16_t outsamps[Bufsize];
      if(drop != 0){
	// previous packet(s) dropped; have codec tell us how much silence to emit
	int samples = opus_decode(sp->opus,NULL,rtp.timestamp - sp->etime,(opus_int16 *)outsamps,sizeof(outsamps),0);
	while(samples-- > 0){
	  assert(sp->write_ptr <= AUD_BUFSIZE -2);
	  sp->audiobuffer[sp->write_ptr++] = 0;
	  sp->audiobuffer[sp->write_ptr++] = 0;
	  if(sp->write_ptr >= AUD_BUFSIZE)
	    sp->write_ptr -= AUD_BUFSIZE;
	}
      }
      samples = opus_decode(sp->opus,(unsigned char *)data,size,(opus_int16 *)outsamps,sizeof(outsamps),0);
      for(int i=0; i < samples; i++){
	assert(sp->write_ptr <= AUD_BUFSIZE -2);
	sp->audiobuffer[sp->write_ptr++] = outsamps[2*i];
	sp->audiobuffer[sp->write_ptr++] = outsamps[2*i+1];
	if(sp->write_ptr >= AUD_BUFSIZE)
	  sp->write_ptr -= AUD_BUFSIZE;
      }
      break;
    default:
      break; // ignore
    }
    sp->etime = rtp.timestamp + samples;

    if(sp->Pa_Stream == NULL) {
      // Create and start portaudio stream
      PaStreamParameters outputParameters;
      memset(&outputParameters,0,sizeof(outputParameters));
      outputParameters.channelCount = 2;
      outputParameters.device = inDevNum;
      outputParameters.sampleFormat = paInt16;
  
#if 0
      r = Pa_OpenStream(&sp->Pa_Stream,NULL,&outputParameters,Samprate,paFramesPerBufferUnspecified,
			paPrimeOutputBuffersUsingStreamCallback,pa_callback,sp);
#else
      r = Pa_OpenStream(&sp->Pa_Stream,NULL,&outputParameters,Samprate,paFramesPerBufferUnspecified,
			0,pa_callback,sp);
#endif      
      if(r != paNoError){
	fprintf(stderr,"Portaudio error: %s\n",Pa_GetErrorText(r));      
	close_session(sp);
	goto endloop;
      }
      r = Pa_SetStreamFinishedCallback(sp->Pa_Stream,pa_complete);
      if(r != paNoError){
	fprintf(stderr,"Portaudio error: %s\n",Pa_GetErrorText(r));      
	close_session(sp);
	goto endloop;
      }
    }
    sp->idle = 0;
    if(Pa_IsStreamStopped(sp->Pa_Stream)){
	r = Pa_StartStream(sp->Pa_Stream);
	if(r != paNoError){
	  fprintf(stderr,"Portaudio error: %s\n",Pa_GetErrorText(r));
	  close_session(sp);
	  goto endloop;
	}
    }
    // Record current clock time so callback can calculate delay
    sp->last_written_time = Pa_GetStreamTime(sp->Pa_Stream);

  endloop:;
    pthread_mutex_unlock(&Audio_mutex); // Give display thread a chance
  }
  pthread_cancel(Display_thread);
  endwin();
  exit(0);
}

// Use ncurses to display streams; age out unused ones
void *display(void *arg){
  initscr();
  // WINDOW * const main = newwin(25,110,0,0);
  WINDOW * const main = stdscr;
  int const agelimit = Clear_interval * 1000000 / Update_interval;

  while(1){
    int row = 1;
    wmove(main,row,0);
    wclrtobot(main);
    mvwprintw(main,row++,1,"Type        channels   BW      SSRC     Packets     Drops   Underruns   Delay   Source");
    pthread_mutex_lock(&Audio_mutex);
    for(int i=0;i<HASHCHAINS;i++){
      struct audio *nextsp; // Save in case we close the current one
      for(struct audio *sp = Audio[i]; sp != NULL; sp = nextsp){
	nextsp = sp->next;
	if(++sp->age > agelimit){
	  // Age out old session
	  Pa_StopStream(sp->Pa_Stream);
	  close_session(sp);
	  continue;
	}
	int bw; // Audio bandwidth (not bitrate) in kHz
	char *type,typebuf[30];
	switch(sp->type){
	case 10:
	case 11:
	  type = "PCM";
	  bw = Samprate / 2000;
	  break;
	case 20:
	  switch(sp->opus_bandwidth){
	  case OPUS_BANDWIDTH_NARROWBAND:
	    bw = 4;
	    break;
	  case OPUS_BANDWIDTH_MEDIUMBAND:
	    bw = 6;
	    break;
	  case OPUS_BANDWIDTH_WIDEBAND:
	    bw = 8;
	    break;
	  case OPUS_BANDWIDTH_SUPERWIDEBAND:
	    bw = 12;
	    break;
	  case OPUS_BANDWIDTH_FULLBAND:
	    bw = 20;
	    break;
	  case OPUS_INVALID_PACKET:
	    bw = 0;
	    break;
	  }
	  snprintf(typebuf,sizeof(typebuf),"Opus %.1lf ms",1000.*sp->opus_frame_size/Samprate);
	  type = typebuf;
	  break;
	default:
	  snprintf(typebuf,sizeof(typebuf),"%d",sp->type);
	  bw = 0; // Unknown
	  type = typebuf;
	  break;
	}
	if(!sp->idle)
	  wattr_on(main,A_BOLD,NULL); // Embolden active streams
       	mvwprintw(main,row++,1,"%-15s%5d%5d%10lx%'12lu%'10u%'12lu%'8.3lf   %s:%s",
		  type,sp->channels,bw,sp->ssrc,sp->packets,sp->drops,sp->underruns,sp->hw_delay,sp->addr,sp->port);
	wattr_off(main,A_BOLD,NULL);
      }
    }
    pthread_mutex_unlock(&Audio_mutex);
    // Draw the box and banner last, to avoid the wclrtobot() calls
    //    box(main,0,0);
    mvwprintw(main,0,15,"KA9Q Multicast Audio Monitor - %s",Mcast_address_text);
    wnoutrefresh(main);
    doupdate();
    usleep(Update_interval);
  }
}

struct audio *lookup_session(const struct sockaddr *sender,const uint32_t ssrc){
  // Walk hash chain
  struct audio *sp;
  for(sp = Audio[ssrc % HASHCHAINS]; sp != NULL; sp = sp->next){
    if(sp->ssrc == ssrc && memcmp(&sp->sender,sender,sizeof(*sender)) == 0){
      // Found it
#if 0
      if(sp->prev != NULL){
	// Not at top of bucket chain; move it there
	if(sp->next != NULL)
	  sp->next->prev = sp->prev;

	sp->prev->next = sp->next;
	sp->prev = NULL;
	sp->next = Audio[ssrc % HASHCHAINS];
	Audio[ssrc % HASHCHAINS] = sp;
      }
#endif
      return sp;
    }
  }
  return NULL;
}
// Create a new session, partly initialize; ssrc is used as hash key
struct audio *make_session(struct sockaddr const *sender,uint32_t ssrc,uint16_t seq,uint32_t timestamp){
  struct audio *sp;

  if((sp = calloc(1,sizeof(*sp))) == NULL)
    return NULL; // Shouldn't happen on modern machines!
  
  // Initialize entry
  memcpy(&sp->sender,sender,sizeof(struct sockaddr));
  sp->ssrc = ssrc;
  sp->eseq = seq;
  sp->etime = timestamp;

  // Put at head of bucket chain
  sp->next = Audio[ssrc % HASHCHAINS];
  if(sp->next != NULL)
    sp->next->prev = sp;
  Audio[ssrc % HASHCHAINS] = sp;
  return sp;
}

int close_session(struct audio *sp){
  if(sp == NULL)
    return -1;
  
  if(sp->Pa_Stream != NULL){
    Pa_CloseStream(&sp->Pa_Stream);
    sp->Pa_Stream = NULL;

  }
  if(sp->opus != NULL){
    opus_decoder_destroy(sp->opus);
    sp->opus = NULL;
  }
  // Remove from linked list
  if(sp->next != NULL)
    sp->next->prev = sp->prev;
  if(sp->prev != NULL)
    sp->prev->next = sp->next;
  else
    Audio[sp->ssrc % HASHCHAINS] = sp->next;
  free(sp);
  return 0;
}
void closedown(int s){
  for(int i=0; i < HASHCHAINS; i++){
    while(Audio[i] != NULL)
      close_session(Audio[i]);
  }
  Pa_Terminate();
  endwin();

  exit(0);
}

// Portaudio callback - transfer data (if any) to provided buffer
// When buffer is empty, pad with silence
static int pa_callback(const void *inputBuffer, void *outputBuffer,
		       unsigned long framesPerBuffer,
		       const PaStreamCallbackTimeInfo* timeInfo,
		       PaStreamCallbackFlags statusFlags,
		       void *userData){
  int16_t *op = outputBuffer;
  struct audio *sp = userData;

  if(sp == NULL || op == NULL)
    return paAbort;
  
  sp->hw_delay = timeInfo->outputBufferDacTime - sp->last_written_time;
  for(int i=0; i<framesPerBuffer; i++){
    if(sp->read_ptr == sp->write_ptr){
      memset(op,0,(framesPerBuffer - i) * 2 * sizeof(*op));
      if(!sp->idle){
	sp->idle = 1;
	sp->underruns++;
      }
      break;
    } else {
      assert(sp->read_ptr <= AUD_BUFSIZE-2); // should be true since buffer is even length and we always read pairs
      *op++ = sp->audiobuffer[sp->read_ptr++];
      *op++ = sp->audiobuffer[sp->read_ptr++];
      if(sp->read_ptr >= AUD_BUFSIZE)
	sp->read_ptr -= AUD_BUFSIZE;
    }
  }
  return paContinue;
}

// Portaudio completion callback
static void pa_complete(void *data){
  struct audio *sp = data;

  Pa_StopStream(&sp->Pa_Stream);
}
