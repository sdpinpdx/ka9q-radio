// $Id: control.c,v 1.11 2018/11/26 05:27:42 karn Exp karn $
// Thread to display internal state of 'radio' and accept single-letter commands
// Why are user interfaces always the biggest, ugliest and buggiest part of any program?
// Copyright 2017 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <math.h>
#include <complex.h>
#undef I
#include <sys/time.h>
#include <ncurses.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netdb.h>
#include <locale.h>

#include "misc.h"
#include "dsp.h"
#include "radio.h"
#include "filter.h"
#include "multicast.h"
#include "bandplan.h"
#include "status.h"

float Spare; // General purpose knob for experiments

// Touch screen position (Raspberry Pi display only - experimental)
int touch_x,touch_y;

int Update_interval = 100;

// Screen location of field modification cursor
int mod_x,mod_y;

int Mcast_ttl;

char Libdir[] = "/usr/local/share/ka9q-radio";
char Locale[256] = "en_US.UTF-8";



void  *demod_am(void *arg){
  return NULL;
}
void *demod_fm(void *arg){
  return NULL;
}
void *demod_linear(void *arg){
  return NULL;  
}



struct demodtab Demodtab[] = {
      {AM_DEMOD,     "AM",     demod_am},     // AM evelope detection
      {FM_DEMOD,     "FM",     demod_fm},     // NBFM and noncoherent PM
      {LINEAR_DEMOD, "Linear", demod_linear}, // Coherent demodulation of AM, DSB, BPSK; calibration on WWV/WWVH/CHU carrier
};
int Ndemod = sizeof(Demodtab)/sizeof(struct demodtab);

// Pop up a temporary window with the contents of a file in the
// library directory (usually /usr/local/share/ka9q-radio/)
// then wait for a single keyboard character to clear it
void popup(const char *filename){
  static const int maxcols = 256;
  char fname[PATH_MAX];
  snprintf(fname,sizeof(fname),"%s/%s",Libdir,filename);
  FILE *fp;
  if((fp = fopen(fname,"r")) == NULL)
    return;
  // Determine size of box
  int rows=0, cols=0;
  char line[maxcols];
  while(fgets(line,sizeof(line),fp) != NULL){
    chomp(line);
    rows++;
    if(strlen(line) > cols)
      cols = strlen(line); // Longest line
  }
  rewind(fp);
  
  // Allow room for box
  WINDOW * const pop = newwin(rows+2,cols+2,0,0);
  box(pop,0,0);
  int row = 1; // Start inside box
  while(fgets(line,sizeof(line),fp) != NULL){
    chomp(line);
    mvwaddstr(pop,row++,1,line);
  }
  fclose(fp);
  wnoutrefresh(pop);
  doupdate();
  timeout(-1); // blocking read - wait indefinitely
  (void)getch(); // Read and discard one character
  timeout(Update_interval);
  werase(pop);
  wrefresh(pop);
  delwin(pop);
}


// Pop up a dialog box, issue a prompt and get a response
void getentry(char const *prompt,char *response,int len){
  WINDOW *pwin = newwin(5,90,15,0);
  box(pwin,0,0);
  mvwaddstr(pwin,1,1,prompt);
  wrefresh(pwin);
  echo();
  timeout(-1);
  // Manpage for wgetnstr doesn't say whether a terminating
  // null is stashed. Hard to believe it isn't, but this is to be sure
  memset(response,0,len);
  wgetnstr(pwin,response,len);
  chomp(response);
  timeout(Update_interval);
  noecho();
  werase(pwin);
  wrefresh(pwin);
  delwin(pwin);
}

static FILE *Tty;
static SCREEN *Term;

void display_cleanup(void){
  echo();
  nocbreak();
  endwin();
  if(Term)
    delscreen(Term);
  Term = NULL;
  if(Tty)
    fclose(Tty);
  Tty = NULL;
}

static int Frequency_lock;

// Adjust the selected item up or down one step
void adjust_item(struct demod *demod,int direction){
  double tunestep;
  
  tunestep = pow(10., (double)demod->tunestep);

  if(!direction)
    tunestep = - tunestep;

  switch(demod->tuneitem){
  case 0: // Carrier frequency
  case 1: // Center frequency - treat the same
    if(!Frequency_lock) // Ignore if locked
      demod->freq += tunestep;
    break;
  case 2: // First LO
    if(demod->tuner_lock) // Tuner is locked, don't change it
      break;

    // Keep frequency but move LO2, which will move LO1 (if it can move)
    double new_lo2 = demod->second_LO + tunestep;
    demod->freq += new_lo2;
    break;
  case 3: // IF
    demod->second_LO = demod->second_LO - tunestep;
    break;
  case 4: // Filter low edge
    demod->low += tunestep;
    break;
  case 5: // Filter high edge
    demod->high += tunestep;
    break;
  case 6: // Post-detection audio frequency shift
    demod->shift += tunestep;
    break;
  case 7: // Kaiser window beta parameter for filter
    demod->kaiser_beta += tunestep;
    if(demod->kaiser_beta < 0)
      demod->kaiser_beta = 0;
    break;
  }
}
// Hooks for knob.c (experimental)
// It seems better to just use the Griffin application to turn knob events into keystrokes or mouse events
void adjust_up(void *arg){
  struct demod *demod = arg;
  adjust_item(demod,1);
}
void adjust_down(void *arg){
  struct demod *demod = arg;
  adjust_item(demod,0);
}
void toggle_lock(void *arg){
  struct demod *demod = arg;
  switch(demod->tuneitem){
  case 0:
  case 1:
    Frequency_lock = !Frequency_lock; // Toggle frequency tuning lock
    break;
  case 2:
    demod->tuner_lock = !demod->tuner_lock;
  }
}


struct demod Demod;
struct audio Audio;

float Noise_bandwidth;


int Netsock;

void decode_status(struct demod *demod,struct audio *audio,unsigned char *buffer,int length){
  unsigned char *cp = buffer;

  while(cp - buffer < length){
    enum status_type type = *cp++; // increment cp to length field
    
    if(type == EOL)
      break; // End of list

    int c;
    unsigned int len = *cp++;
    if(cp - buffer + len >= length)
      break; // Invalid length
    switch(type){
    case EOL: // Shouldn't get here
      goto done;
    case TYPE:
      break;
    case INPUT_SOURCE_SOCKET:
      if(len == 6){
	struct sockaddr_in *sin;
	sin = (struct sockaddr_in *)&demod->input_source_address;
	sin->sin_family = AF_INET;
	memcpy(&sin->sin_addr.s_addr,cp,4);
	memcpy(&sin->sin_port,cp+4,2);
      } else if(len == 8){
	struct sockaddr_in6 *sin6;
	sin6 = (struct sockaddr_in6 *)&demod->input_source_address;
	sin6->sin6_family = AF_INET6;
	memcpy(&sin6->sin6_addr,cp,8);
	memcpy(&sin6->sin6_port,cp+8,2);
      }
      break;
#if 0
    case INPUT_DEST_SOCKET:
      if(len == 6){
	struct sockaddr_in *sin;
	sin = (struct sockaddr_in *)&demod->input_dest_address;
	sin->sin_family = AF_INET;
	memcpy(&sin->sin_addr.s_addr,cp,4);
	memcpy(&sin->sin_port,cp+4,2);
      } else if(len == 8){
	struct sockaddr_in6 *sin6;
	sin6 = (struct sockaddr_in6 *)&demod->input_dest_address;
	sin6->sin6_family = AF_INET6;
	memcpy(&sin6->sin6_addr,8);
	memcpy(&sin6->sin6_port,cp+8,2);
      }
      break;
#endif
    case INPUT_SSRC:
      demod->rtp_state.ssrc = decode_int(cp,len);
      break;
    case INPUT_SAMPRATE:
      demod->samprate = decode_double(cp,len);
      break;
    case INPUT_PACKETS:
      demod->rtp_state.packets = decode_int(cp,len);
      break;
    case INPUT_SAMPLES:
      demod->samples = decode_int(cp,len);
      break;
    case INPUT_DROPS:
      demod->rtp_state.drops = decode_int(cp,len);
      break;
    case INPUT_DUPES:
      demod->rtp_state.dupes = decode_int(cp,len);
      break;
#if 0
    case OUTPUT_DEST_SOCKET:
      if(len == 6){
	struct sockaddr_in *sin;
	sin = (struct sockaddr_in *)&demod->input_dest_address;
	sin->sin_family = AF_INET;
	memcpy(&sin->sin_addr.s_addr,cp,4);
	memcpy(&sin->sin_port,cp+4,2);
      } else if(len == 8){
	struct sockaddr_in6 *sin6;
	sin6 = (struct sockaddr_in6 *)&demod->input_dest_address;
	sin6->sin6_family = AF_INET6;
	memcpy(&sin6->sin6_addr,8);
	memcpy(&sin6->sin6_port,cp+8,2);
      }
      break;
#endif
    case OUTPUT_SSRC:
      audio->rtp.ssrc = decode_int(cp,len);
      break;
    case OUTPUT_TTL:
      Mcast_ttl = decode_int(cp,len);
      break;
    case OUTPUT_SAMPRATE:
      audio->samprate = decode_int(cp,len);
      break;
    case OUTPUT_PACKETS:
      audio->rtp.packets = decode_int(cp,len);
      break;
    case RADIO_FREQUENCY:
      demod->freq = decode_double(cp,len);
      break;
    case FIRST_LO_FREQUENCY:
      demod->status.frequency = decode_double(cp,len);
      break;
    case SECOND_LO_FREQUENCY:
      demod->second_LO = decode_double(cp,len);
      break;
    case SHIFT_FREQUENCY:
      demod->shift = decode_double(cp,len);
      break;
    case DOPPLER_FREQUENCY:
      demod->doppler = decode_double(cp,len);
      break;
    case DOPPLER_FREQUENCY_RATE:
      demod->doppler_rate = decode_double(cp,len);
      break;
    case LNA_GAIN:
      demod->status.lna_gain = decode_int(cp,len);
      break;
    case MIXER_GAIN:
      demod->status.mixer_gain = decode_int(cp,len);
      break;
    case IF_GAIN:
      demod->status.if_gain = decode_int(cp,len);
      break;
    case AD_LEVEL:
      demod->level = decode_float(cp,len);
      break;
    case DC_I_OFFSET:
      break;
    case DC_Q_OFFSET:
      break;
    case IQ_IMBALANCE:
      break;
    case IQ_PHASE:
      break;
    case LOW_EDGE:
      demod->low = decode_float(cp,len);
      break;
    case HIGH_EDGE:
      demod->high = decode_float(cp,len);
      break;
    case KAISER_BETA:
      demod->kaiser_beta = decode_float(cp,len);
      break;
    case FILTER_BLOCKSIZE:
      demod->L = decode_int(cp,len);
      break;
    case FILTER_FIR_LENGTH:
      demod->M = decode_int(cp,len);
      break;
    case NOISE_BANDWIDTH:
      Noise_bandwidth = decode_float(cp,len);
      break;
    case BASEBAND_POWER:
      demod->bb_power = decode_float(cp,len);
      break;
    case NOISE_DENSITY:
      demod->n0 = decode_float(cp,len);
      break;
    case RADIO_MODE:
      strncpy(demod->mode,(char *)cp,sizeof(demod->mode));
      demod->mode[len] = '\0';
      break;
    case DEMOD_MODE:
      demod->demod_index = decode_int(cp,len); // ????
      break;
    case INDEPENDENT_SIDEBAND:
      c = decode_int(cp,len);
      if(c)
	demod->flags |= ISB;
      else
	demod->flags &= ~ISB;
      break;
    case DEMOD_SNR:
      demod->snr = decode_float(cp,len);
      break;
    case DEMOD_GAIN:
      demod->gain = decode_float(cp,len);
      break;
    case FREQ_OFFSET:
      demod->foffset = decode_float(cp,len);
      break;
    case PEAK_DEVIATION:
      demod->pdeviation = decode_float(cp,len);
      break;
    case PL_TONE:
      demod->plfreq = decode_float(cp,len);
      break;
    case PLL_LOCK:
      c = decode_int(cp,len);
      if(c)
	demod->spare = 48000; // Clean this up
      else
	demod->spare = -48000;
      break;
    case PLL_SQUARE:
      c = decode_int(cp,len);
      if(c)
	demod->flags |= SQUARE;
      else
	demod->flags &= ~SQUARE;
      break;
    case PLL_PHASE:
      demod->cphase = decode_float(cp,len);
      break;
    case OUTPUT_CHANNELS:
      c = decode_int(cp,len);
      if(c == 1)
	demod->flags |= MONO;
      else
	demod->flags &= ~MONO;
      break;
    default:
      break;
    }
    cp += len;
  }
 done:;
}




// Thread to display receiver state, updated at 10Hz by default
// Uses the ancient ncurses text windowing library
// Also services keyboard, mouse and tuning knob, if present
// I had been running this at normal priority, but it can start new demodulators
// so it must also run at preferred priority
int main(int argc,char *argv[]){


  {
    // The display thread assumes en_US.UTF-8, or anything with a thousands grouping character
    // Otherwise the cursor movements will be wrong
    char const * const cp = getenv("LANG");
    if(cp != NULL){
      strlcpy(Locale,cp,sizeof(Locale));
    }
  }
  setlocale(LC_ALL,Locale); // Set either the hardwired default or the value of $LANG if it exists


  struct demod * const demod = &Demod;
  struct audio * const audio = &Audio; // Eventually make parameter
  Netsock = setup_mcast("239.1.1.2:5006",0,1,0);
  fprintf(stderr,"Netsock %d\n",Netsock);


#if 0
#ifdef linux
  extern void *knob(void *);
  pthread_t pthread_knob;
  pthread_create(&pthread_knob,NULL,knob,demod);

  extern void *touch(void *);
  pthread_t pthread_touch;
  pthread_create(&pthread_touch,NULL,touch,demod);   // Disable for now
#endif
#endif

  atexit(display_cleanup);

  // talk directly to the terminal
  Tty = fopen("/dev/tty","r+");
  Term = newterm(NULL,Tty,Tty);
  set_term(Term);
  keypad(stdscr,TRUE);
  timeout(Update_interval); // update interval when nothing is typed
  cbreak();
  noecho();

  // Set up display subwindows
  int row = 0;
  int col = 0;
  WINDOW * const tuning = newwin(8,35,row,col);    // Frequency information
  col += 35;
  WINDOW * const sig = newwin(8,25,row,col); // Signal information
  col += 25;
  WINDOW * const info = newwin(8,42,row,col);     // Band information
  row += 8;
  col = 0;
  WINDOW * const filtering = newwin(12,22,row,col);
  col += 22;
  WINDOW * const demodulator = newwin(12,25,row,col);
  col += 25;
  WINDOW * const options = newwin(12,12,row,col); // Demod options
  col += 12;
  WINDOW * const sdr = newwin(12,25,row,col); // SDR information
  col += 25;

  WINDOW * const modes = newwin(Nmodes+2,7,row,col);
  col += Nmodes+2;

  col = 0;
  row += 12;
  WINDOW * const network = newwin(8,78,row,col); // Network status information
  col = 0;
  row += 8;
  WINDOW * const debug = newwin(8,78,row,col); // Note: overlaps function keys
  scrollok(debug,1);

  // A message from our sponsor...
  wprintw(debug,"KA9Q SDR Receiver v1.0; Copyright 2017-2018 Phil Karn\n");
  wprintw(debug,"Compiled on %s at %s\n",__DATE__,__TIME__);


  struct sockaddr old_input_source_address;
  memset(&old_input_source_address,0,sizeof(old_input_source_address));
  char source[NI_MAXHOST];
  char sport[NI_MAXSERV];
  memset(source,0,sizeof(source));
  memset(sport,0,sizeof(sport));

  mmask_t mask = ALL_MOUSE_EVENTS;
  mousemask(mask,NULL);
  MEVENT mouse_event;

  for(;;){
    unsigned char buffer[8192];

    memset(buffer,0,sizeof(buffer));
    int n = recv(Netsock,buffer,sizeof(buffer),0);
    if(n <= 0){
      sleep(1);
      continue;
    }
    // Parse entries
    decode_status(demod,audio,buffer,sizeof(buffer));

    // update display indefinitely, handle user commands

    // Tuning control window - these can be adjusted by the user
    // using the keyboard or tuning knob, so be careful with formatting
    wmove(tuning,0,0);
    int row = 1;
    int col = 1;
    if(Frequency_lock)
      wattron(tuning,A_UNDERLINE); // Underscore means the frequency is locked
    mvwprintw(tuning,row,col,"%'28.3f Hz",demod->freq); // RF carrier frequency
    mvwaddstr(tuning,row,col,"Carrier");
    row++;

    // Center of passband
    mvwprintw(tuning,row,col,"%'28.3f Hz",demod->freq + (demod->high + demod->low)/2);
    mvwaddstr(tuning,row++,col,"Center");

    wattroff(tuning,A_UNDERLINE);
    if(demod->tuner_lock)
      wattron(tuning,A_UNDERLINE);    

    // second LO frequency is negative of IF, i.e., a signal at +48 kHz
    // needs a second LO frequency of -48 kHz to bring it to zero
    mvwprintw(tuning,row,col,"%'28.3f Hz",demod->status.frequency);
    mvwaddstr(tuning,row++,col,"First LO");
    wattroff(tuning,A_UNDERLINE);

    mvwprintw(tuning,row,col,"%'28.3f Hz",-demod->second_LO);
    mvwaddstr(tuning,row++,col,"IF");

    // Doppler info displayed only if active
    double dopp = demod->doppler;
    if(dopp != 0){
      mvwprintw(tuning,row,col,"%'28.3f Hz",dopp);
      mvwaddstr(tuning,row++,col,"Doppler");
      mvwprintw(tuning,row,col,"%'28.3f Hz/s",demod->doppler_rate);
      mvwaddstr(tuning,row++,col,"Dop rate");
    }
    wmove(tuning,row,0);
    wclrtobot(tuning);

    box(tuning,0,0);
    mvwaddstr(tuning,0,15,"Tuning");


    // Display ham band emission data, if available
    // Lines are variable length, so clear window before starting
    wclrtobot(info);  // Output 
    row = 1;
    mvwprintw(info,row++,1,"Receiver profile: %s",demod->mode);

    if(demod->doppler_command)
      mvwprintw(info,row++,1,"Doppler: %s",demod->doppler_command);

    struct bandplan const *bp_low,*bp_high;
    bp_low = lookup_frequency(demod->freq + demod->low);
    bp_high = lookup_frequency(demod->freq + demod->high);
    // Make sure entire receiver passband is in the band
    if(bp_low != NULL && bp_high != NULL){
      struct bandplan r;

      // If the passband straddles a mode or class boundary, form
      // the intersection to give the more restrictive answers
      r.classes = bp_low->classes & bp_high->classes;
      r.modes = bp_low->modes & bp_high->modes;

      mvwprintw(info,row++,1,"Band: %s",bp_low->name);

      if(r.modes){
	mvwaddstr(info,row++,1,"Emissions: ");
	if(r.modes & VOICE)
	  waddstr(info,"Voice ");
	if(r.modes & IMAGE)
	  waddstr(info,"Image ");
	if(r.modes & DATA)
	  waddstr(info,"Data ");
	if(r.modes & CW)
	  waddstr(info,"CW "); // Last since it's permitted almost everywhere
      }
      if(r.classes){
	mvwaddstr(info,row++,1,"Privs: ");
	if(r.classes & EXTRA_CLASS)
	  waddstr(info,"Extra ");
	if(r.classes & ADVANCED_CLASS)
	  waddstr(info,"Adv ");
	if(r.classes & GENERAL_CLASS)
	  waddstr(info,"Gen ");
	if(r.classes & TECHNICIAN_CLASS)
	  waddstr(info,"Tech ");
	if(r.classes & NOVICE_CLASS)
	  waddstr(info,"Nov ");
      }
    }
    box(info,0,0);
    mvwaddstr(info,0,17,"Info");


    int const N = demod->L + demod->M - 1;
    // Filter window values
    row = 1;
    col = 1;
    mvwprintw(filtering,row,col,"%'+17.3f Hz",demod->low);
    mvwaddstr(filtering,row++,col,"Low");
    mvwprintw(filtering,row,col,"%'+17.3f Hz",demod->high);
    mvwaddstr(filtering,row++,col,"High");    
    mvwprintw(filtering,row,col,"%'+17.3f Hz",demod->shift);
    mvwaddstr(filtering,row++,col,"Shift");
    mvwprintw(filtering,row,col,"%'17.3f",demod->kaiser_beta);
    mvwaddstr(filtering,row++,col,"Beta");    
    mvwprintw(filtering,row,col,"%'17d",demod->L);
    mvwaddstr(filtering,row++,col,"Blocksize");
    mvwprintw(filtering,row,col,"%'17d",demod->M);
    mvwaddstr(filtering,row++,col,"FIR");
    mvwprintw(filtering,row,col,"%'17.3f Hz",demod->samprate / N);
    mvwaddstr(filtering,row++,col,"Freq bin");
    mvwprintw(filtering,row,col,"%'17.3f ms",1000*(N - (demod->M - 1)/2)/demod->samprate); // Is this correct?
    mvwaddstr(filtering,row++,col,"Delay");
    mvwprintw(filtering,row,col,"%17d",demod->interpolate);
    mvwaddstr(filtering,row++,col,"Interpolate");
    mvwprintw(filtering,row,col,"%17d",demod->decimate);
    mvwaddstr(filtering,row++,col,"Decimate");

    box(filtering,0,0);
    mvwaddstr(filtering,0,6,"Filtering");


    // Signal data window
    float bw = Noise_bandwidth;
    float sn0 = demod->bb_power / demod->n0 - bw;
    sn0 = max(sn0,0.0f); // Can go negative due to inconsistent smoothed values; clip it at zero

    row = 1;
    col = 1;
    mvwprintw(sig,row,col,"%15.1f dB",
	      power2dB(demod->level) - (demod->status.lna_gain + demod->status.if_gain + demod->status.mixer_gain));
    mvwaddstr(sig,row++,col,"IF");
    mvwprintw(sig,row,col,"%15.1f dB",power2dB(demod->bb_power));
    mvwaddstr(sig,row++,col,"Baseband");
    mvwprintw(sig,row,col,"%15.1f dB/Hz",power2dB(demod->n0));
    mvwaddstr(sig,row++,col,"N0");
    mvwprintw(sig,row,col,"%15.1f dBHz",10*log10f(sn0));
    mvwaddstr(sig,row++,col,"S/N0");
    mvwprintw(sig,row,col,"%15.1f dBHz",10*log10f(bw));
    mvwaddstr(sig,row++,col,"NBW");
    mvwprintw(sig,row,col,"%15.1f dB",10*log10f(sn0/bw));
    mvwaddstr(sig,row++,col,"SNR");
    box(sig,0,0);
    mvwaddstr(sig,0,9,"Signal");


    // Demodulator info
    wmove(demodulator,0,0);
    wclrtobot(demodulator);    
    row = 1;
    int rcol = 9;
    int lcol = 1;
    // Display only if used by current mode
    if(demod->snr >= 0){
      mvwprintw(demodulator,row,rcol,"%11.1f dB",power2dB(demod->snr));
      mvwaddstr(demodulator,row++,lcol,"Loop SNR");
    }
    if(demod->gain >= 0){
      mvwprintw(demodulator,row,rcol,"%11.1f dB",voltage2dB(demod->gain));
      mvwaddstr(demodulator,row++,lcol,"AF Gain");
    }    
    if(!isnan(demod->foffset)){
      mvwprintw(demodulator,row,rcol,"%'+11.3f Hz",demod->foffset);
      mvwaddstr(demodulator,row++,lcol,"Offset");
    }
    if(!isnan(demod->pdeviation)){
      mvwprintw(demodulator,row,rcol,"%11.1f Hz",demod->pdeviation);
      mvwaddstr(demodulator,row++,lcol,"Deviation");
    }
    if(!isnan(demod->cphase)){
      mvwprintw(demodulator,row,rcol,"%+11.1f deg",demod->cphase*DEGPRA);
      mvwaddstr(demodulator,row++,lcol,"Phase");
    }
    if(!isnan(demod->plfreq)){
      mvwprintw(demodulator,row,rcol,"%11.1f Hz",demod->plfreq);
      mvwaddstr(demodulator,row++,lcol,"Tone");
    }
    if(!isnan(demod->spare)){
      mvwprintw(demodulator,row,rcol,"%11.1f",demod->spare);      
      mvwaddstr(demodulator,row++,lcol,"Spare");
    }
    box(demodulator,0,0);
    mvwprintw(demodulator,0,5,"%s demodulator",Demodtab[demod->demod_index].name);


    // SDR hardware status: sample rate, tcxo offset, I/Q offset and imbalance, gain settings
    row = 1;
    col = 1;
    mvwprintw(sdr,row,col,"%'18d Hz",demod->status.samprate); // Nominal
    mvwaddstr(sdr,row++,col,"Samprate");
    mvwprintw(sdr,row,col,"%'18.1f dBFS",power2dB(demod->level));
    mvwprintw(sdr,row++,col,"A/D Level");
    mvwprintw(sdr,row,col,"%+18.6f",demod->DC_i);  // Scaled to +/-1
    mvwaddstr(sdr,row++,col,"I offset");
    mvwprintw(sdr,row,col,"%+18.6f",demod->DC_q);
    mvwaddstr(sdr,row++,col,"Q offset");
    mvwprintw(sdr,row,col,"%+18.3f dB",power2dB(demod->imbalance));
    mvwaddstr(sdr,row++,col,"I/Q imbal");
    mvwprintw(sdr,row,col,"%+18.1f deg",demod->sinphi*DEGPRA);
    mvwaddstr(sdr,row++,col,"I/Q phi");
    mvwprintw(sdr,row,col,"%18u dB",demod->status.lna_gain);   // SDR dependent
    mvwaddstr(sdr,row++,col,"LNA gain");
    mvwprintw(sdr,row,col,"%18u dB",demod->status.mixer_gain); // SDR dependent
    mvwaddstr(sdr,row++,col,"Mix gain");
    mvwprintw(sdr,row,col,"%18u dB",demod->status.if_gain); // SDR dependent    
    mvwaddstr(sdr,row++,col,"IF gain");
    box(sdr,0,0);
    mvwaddstr(sdr,0,6,"SDR Hardware");


    // Demodulator options, can be set with mouse
    row = 1;
    col = 1;
    if(demod->flags & ISB)
      wattron(options,A_UNDERLINE);
    mvwprintw(options,row++,col,"ISB");
    wattroff(options,A_UNDERLINE);

    if(demod->flags & PLL)
      wattron(options,A_UNDERLINE);      
    mvwprintw(options,row++,col,"PLL");
    wattroff(options,A_UNDERLINE);

    if(demod->flags & SQUARE)
      wattron(options,A_UNDERLINE);            
    mvwprintw(options,row++,col,"Square");
    wattroff(options,A_UNDERLINE);

    if(demod->flags & MONO)
      wattron(options,A_UNDERLINE);
    mvwprintw(options,row++,col,"Mono");    
    wattroff(options,A_UNDERLINE);

    if(!(demod->flags & MONO))
      wattron(options,A_UNDERLINE);
    mvwprintw(options,row++,col,"Stereo");    
    wattroff(options,A_UNDERLINE);
    
    box(options,0,0);
    mvwaddstr(options,0,2,"Options");


    // Display list of modes defined in /usr/local/share/ka9q-radio/modes.txt
    // Underline the active one
    // Can be selected with mouse
    row = 1; col = 1;
    for(int i=0;i<Nmodes;i++){
      if(strcasecmp(demod->mode,Modes[i].name) == 0)
	wattron(modes,A_UNDERLINE);
      mvwaddstr(modes,row++,col,Modes[i].name);
      wattroff(modes,A_UNDERLINE);
    }
    box(modes,0,0);
    mvwaddstr(modes,0,1,"Modes");


    // Network status window
    if(memcmp(&old_input_source_address,&demod->input_source_address,sizeof(old_input_source_address)) != 0){
      // First time, or source has changed
      memcpy(&old_input_source_address,&demod->input_source_address,sizeof(old_input_source_address));
      getnameinfo((struct sockaddr *)&demod->input_source_address,sizeof(demod->input_source_address),
		  source,sizeof(source),
		  sport,sizeof(sport),NI_NOFQDN|NI_DGRAM|NI_NUMERICHOST);
    }
    row = 1;
    col = 1;
    wmove(network,0,0);
    wclrtobot(network);
    mvwprintw(network,row++,col,"Source: %s:%s -> %s SSRC %0lx",source,sport,demod->iq_mcast_address_text,demod->rtp_state.ssrc);


    mvwprintw(network,row++,col,"IQ pkts %'llu samples %'llu",
	      demod->rtp_state.packets,demod->samples);
    if(demod->rtp_state.drops)
      wprintw(network," drops %'llu",demod->rtp_state.drops);
    if(demod->rtp_state.dupes)
      wprintw(network," dupes %'llu",demod->rtp_state.dupes);

    mvwprintw(network,row++,col,"Time: %s",lltime(demod->status.timestamp));
    mvwprintw(network,row++,col,"Sink: %s; ssrc %8x; TTL %d%s",audio->audio_mcast_address_text,
	      Audio.rtp.ssrc,Mcast_ttl,Mcast_ttl == 0 ? " (Local host only)":"");
    mvwprintw(network,row++,col,"PCM %'d Hz; pkts %'llu",audio->samprate,audio->rtp.packets);

    box(network,0,0);
    mvwaddstr(network,0,35,"I/O");

    touchwin(debug); // since we're not redrawing it every cycle

    // Highlight cursor for tuning step
    // A little messy because of the commas in the frequencies
    // They come from the ' option in the printf formats
    // tunestep is the log10 of the digit position (0 = units)
    int hcol;
    if(demod->tunestep >= 0){
      hcol = demod->tunestep + demod->tunestep/3;
      hcol = -hcol;
    } else {
      hcol = -demod->tunestep;
      hcol = 1 + hcol + (hcol-1)/3; // 1 for the decimal point, and extras if there were commas in more than 3 places
    }
    switch(demod->tuneitem){
    case 0:
    case 1:
    case 2:
    case 3:
      mod_y = demod->tuneitem + 1;
      mod_x = 24 + hcol; // units in column 24
      mvwchgat(tuning,mod_y,mod_x,1,A_STANDOUT,0,NULL);
      break;
    case 4:
    case 5:
    case 6:
    case 7:
      mod_y = demod->tuneitem - 3;
      mod_x = 13 + hcol; // units in column 13
      mvwchgat(filtering,mod_y,mod_x,1,A_STANDOUT,0,NULL);
      break;
    default:
      ;
      break;
    }
    wnoutrefresh(tuning);
    wnoutrefresh(debug);
    wnoutrefresh(info);
    wnoutrefresh(filtering);
    wnoutrefresh(sig);
    wnoutrefresh(demodulator);
    wnoutrefresh(sdr);
    wnoutrefresh(options);
    wnoutrefresh(modes);
    wnoutrefresh(network);
    doupdate();      // Update the screen right before we pause
    
    // Scan and process keyboard commands
    int c = getch(); // read keyboard with timeout; controls refresh rate

    switch(c){
    case KEY_MOUSE: // Mouse event
      getmouse(&mouse_event);
      break;
    case ERR:   // no key; timed out. Do nothing.
      break;
    case 'q':   // Exit entire radio program. Should this be removed? ^C also works.
      goto done;
    case 'h':
    case '?':
      popup("help.txt");
      break;
    case 'I': // Change multicast address for input I/Q stream
      {
	char str[160];
	getentry("IQ input IP dest address: ",str,sizeof(str));
	if(strlen(str) <= 0)
	  break;

	int const i = setup_mcast(str,0,0,0);
	if(i == -1){
	  beep();
	  break;
	}
	// demod->input_fd is not protected by a mutex, so swap it carefully
	// Mutex protection would be difficult because input thread is usually
	// blocked on the socket, and if there's no I/Q input we'd hang
	int const j = demod->input_fd;
	demod->input_fd = i;
	if(j != -1)
	  close(j); // This should cause the input thread to see an error
	strlcpy(demod->iq_mcast_address_text,str,sizeof(demod->iq_mcast_address_text));
	// Clear RTP receiver state
	memset(&demod->rtp_state,0,sizeof(demod->rtp_state));
      }
      break;
    case 'l': // Toggle RF or first LO lock; affects how adjustments to LO and IF behave
      toggle_lock(demod);
      break;
    case KEY_NPAGE: // Page Down/tab key
    case '\t':      // go to next tuning item
      demod->tuneitem = (demod->tuneitem + 1) % 8;
      break;
    case KEY_BTAB:  // Page Up/Backtab, i.e., shifted tab:
    case KEY_PPAGE: // go to previous tuning item
      demod->tuneitem = (8 + demod->tuneitem - 1) % 8;
      break;
    case KEY_HOME: // Go back to item 0
      demod->tuneitem = 0;
      demod->tunestep = 0;
      break;
    case KEY_BACKSPACE: // Cursor left: increase tuning step 10x
    case KEY_LEFT:
      if(demod->tunestep >= 9){
	beep();
	break;
      }
      demod->tunestep++;
      break;
    case KEY_RIGHT:     // Cursor right: decrease tuning step /10
      if(demod->tunestep <= -3){
	beep();
	break;
      }
      demod->tunestep--;
      break;
    case KEY_UP:        // Increase whatever digit we're tuning
      adjust_up(demod);
      break;
    case KEY_DOWN:      // Decrease whatever we're tuning
      adjust_down(demod);
      break;
    case '\f':  // Screen repaint (formfeed, aka control-L)
      clearok(curscr,TRUE);
      break;
    case 'b':   // Blocksize - sets both data and impulse response-1
                // They should be separably set. Do this in the state file for now
      {
	char str[160],*ptr;
	getentry("Enter blocksize in samples: ",str,sizeof(str));
	int const i = strtol(str,&ptr,0);
	if(ptr == str)
	  break; // Nothing entered
	
	demod->L = i;
	demod->M = demod->L + 1;
      }
      break;
    case 'm': // Manually set modulation mode
      {
	char str[1024];
	snprintf(str,sizeof(str),"Enter mode [ ");
	for(int i=0;i < Nmodes;i++){
	  strlcat(str,Modes[i].name,sizeof(str) - strlen(str) - 1);
	  strlcat(str," ",sizeof(str) - strlen(str) - 1);
	}
	strlcat(str,"]: ",sizeof(str) - strlen(str) - 1);
	getentry(str,str,sizeof(str));
	if(strlen(str) <= 0)
	  break;
	strncpy(demod->mode,str,sizeof(demod->mode));
      }
      break;
    case 'f':   // Tune to new radio frequency
      {
	char str[160];
	getentry("Enter carrier frequency: ",str,sizeof(str));
	double const f = parse_frequency(str); // Handles funky forms like 147m435
	if(f <= 0)
	  break; // Invalid

	// If frequency would be out of range, guess kHz or MHz
	if(f >= 0.1 && f < 100)
	  demod->freq = f*1e6; // 0.1 - 99.999 Only MHz can be valid
	else if(f < 500)         // 100-499.999 could be kHz or MHz, assume MHz
	  demod->freq = f*1e6;
	else if(f < 2000)        // 500-1999.999 could be kHz or MHz, assume kHz
	  demod->freq = f*1e3;
	else if(f < 100000)      // 2000-99999.999 can only be kHz
	  demod->freq = f*1e3;
	else                     // accept directly
	  demod->freq = f;
      }
      break;
    case 'i':    // Recenter IF to +/- samprate/4
      demod->freq += demod->samprate/4;
      break;
    case 'u':    // Set display update rate in milliseconds (minimum 50, i.e, 20 Hz)
      {
	char str[160],*ptr;
	getentry("Enter update interval, ms [<=0 means no auto update]: ",str,sizeof(str));
	int const u = strtol(str,&ptr,0);
	if(ptr == str)
	  break; // Nothing entered
	
	if(u > 50){
	  Update_interval = u;
	  timeout(Update_interval);
	} else if(u <= 0){
	  Update_interval = -1; // No automatic update
	  timeout(Update_interval);
	} else
	  beep();
      }
      break;
    case 'k': // Kaiser window beta parameter
      {
	char str[160],*ptr;
	getentry("Enter Kaiser window beta: ",str,sizeof(str));
	double const b = strtod(str,&ptr);
	if(ptr == str)
	  break; // nothing entered
	if(b < 0 || b >= 100){
	  beep();
	  break; // beyond limits
	}
	if(b != demod->kaiser_beta){
	  demod->kaiser_beta = b;
	}
      }
      break;
    case 'o': // Set/clear option flags, most apply only to linear detector
      {
	char str[160];
	getentry("Enter option [isb pll cal flat square stereo mono], '!' prefix disables: ",str,sizeof(str));
	if(strcasecmp(str,"mono") == 0){
	  demod->flags |= MONO;
	} else if(strcasecmp(str,"!mono") == 0){
	  demod->flags &= ~MONO;
	} else if(strcasecmp(str,"stereo") == 0){
	  demod->flags &= ~MONO;	  
	} else if(strcasecmp(str,"isb") == 0){
	  demod->flags |= ISB;
	} else if(strcasecmp(str,"!isb") == 0){
	  demod->flags &= ~ISB;
	} else if(strcasecmp(str,"pll") == 0){
	  demod->flags |= PLL;
	} else if(strcasecmp(str,"!pll") == 0){
	  demod->flags &= ~(PLL|SQUARE);
	} else if(strcasecmp(str,"square") == 0){
	  demod->flags |= SQUARE|PLL;
	} else if(strcasecmp(str,"!square") == 0){	  
	  demod->flags &= ~SQUARE;
	} else if(strcasecmp(str,"flat") == 0){
	  demod->flags |= FLAT;
	} else if(strcasecmp(str,"!flat") == 0){
	  demod->flags &= ~FLAT;
	}
      }
      break;
    default:
      beep();
      break;
    }
    // Process mouse events
    // Need to handle the wheel as equivalent to up/down arrows
    int mx,my;
    mx = mouse_event.x;
    my = mouse_event.y;
    mouse_event.y = mouse_event.x = mouse_event.z = 0;
    if(mx != 0 && my != 0){
#if 0
      wprintw(debug," (%d %d)",mx,my);
#endif
      if(wmouse_trafo(tuning,&my,&mx,false)){
	// Tuning window
	demod->tuneitem = my-1;
	demod->tunestep = 24-mx;
	if(demod->tunestep < 0)
	  demod->tunestep++;
	if(demod->tunestep > 3)
	  demod->tunestep--;
	if(demod->tunestep > 6)
	  demod->tunestep--;
	if(demod->tunestep > 9)	
	  demod->tunestep--;
	// Clamp to range
	if(demod->tunestep < -3)
	  demod->tunestep = -3;
	if(demod->tunestep > 9)
	  demod->tunestep = 9;

      } else if(wmouse_trafo(filtering,&my,&mx,false)){
	// Filter window
	demod->tuneitem = my + 3;
	demod->tunestep = 13-mx;
	if(demod->tunestep < 0)
	  demod->tunestep++;
	if(demod->tunestep > 3)
	  demod->tunestep--;
	if(demod->tunestep > 6)
	  demod->tunestep--;
	if(demod->tunestep > 9)	
	  demod->tunestep--;
	// Clamp to range
	if(demod->tunestep < -3)
	  demod->tunestep = -3;
	if(demod->tunestep > 5)
	  demod->tunestep = 5;
      } else if(wmouse_trafo(modes,&my,&mx,false)){
	// In the modes window?
	my--;
	if(my >= 0 && my < Nmodes){
	  strncpy(demod->mode,Modes[my].name,sizeof(demod->mode));
	}
      } else if(wmouse_trafo(options,&my,&mx,false)){
	// In the options window
	switch(my){
	case 1:
	  demod->flags ^= ISB;
	  break;
	case 2:
	  demod->flags ^= PLL;
	  break;
	case 3:
	  demod->flags ^= SQUARE;
	  if(demod->flags & SQUARE)
	    demod->flags |= PLL;
	  break;
	case 4:
	  demod->flags |= MONO;
	  break;
	case 5:
	  demod->flags &= ~MONO;
	  break;
	}
      }
    }
  }
 done:;
  endwin();
  set_term(NULL);
  if(Term != NULL)
    delscreen(Term);
  //  if(Tty != NULL)
  //    fclose(Tty);
  
  exit(0);
}


// character size 16 pix high x 9 wide??
void touchitem(void *arg,int x,int y,int ev){
  touch_x = x /8;
  touch_y = y / 16;
}

// Parse a frequency entry in the form
// 12345 (12345 Hz)
// 12k345 (12.345 kHz)
// 12m345 (12.345 MHz)
// 12g345 (12.345 GHz)
// If no g/m/k and number is too small, make a heuristic guess
// NB! This assumes radio covers 100 kHz - 2 GHz; should make more general
double const parse_frequency(const char *s){
  char * const ss = alloca(strlen(s));

  int i;
  for(i=0;i<strlen(s);i++)
    ss[i] = tolower(s[i]);

  ss[i] = '\0';
  
  // k, m or g in place of decimal point indicates scaling by 1k, 1M or 1G
  char *sp;
  double mult;
  if((sp = strchr(ss,'g')) != NULL){
    mult = 1e9;
    *sp = '.';
  } else if((sp = strchr(ss,'m')) != NULL){
    mult = 1e6;
    *sp = '.';
  } else if((sp = strchr(ss,'k')) != NULL){
    mult = 1e3;
    *sp = '.';
  } else
    mult = 1;

  char *endptr = NULL;
  double f = strtod(ss,&endptr);
  if(endptr == ss || f == 0)
    return 0; // Empty entry, or nothing decipherable
  
  if(mult != 1 || f >= 1e5) // If multiplier given, or frequency >= 100 kHz (lower limit), return as-is
    return f * mult;
    
  // If frequency would be out of range, guess kHz or MHz
  if(f < 100)
    f *= 1e6;              // 0.1 - 99.999 Only MHz can be valid
  else if(f < 500)         // Could be kHz or MHz, arbitrarily assume MHz
    f *= 1e6;
  else if(f < 2000)        // Could be kHz or MHz, arbitarily assume kHz
    f *= 1e3;
  else if(f < 100000)      // Can only be kHz
    f *= 1e3;

  return f;
}

