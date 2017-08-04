// $Id: radio.h,v 1.35 2017/08/04 03:35:55 karn Exp karn $
#ifndef _RADIO_H
#define _RADIO_H 1

#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <complex.h>
#undef I

#include <sys/types.h>
#include <sys/socket.h>


// Receiver control input packet
enum cmd {
  SENDSTAT=1,
  SETSTATE,
};
// Receiver modes
enum mode {
  NO_MODE = 0,
  AM,
  CAM,     // coherent AM
  IQ,
  ISB,
  USB,
  CWU,
  LSB,
  CWL,
  NFM,
  FM,
  DSB,
  WFM,
};

struct command {
  enum cmd cmd;
  enum mode mode;
  double first_LO;
  double second_LO;
  double second_LO_rate;
  double calibrate;
};

struct modetab {
  enum mode mode;
  char name[10];
  float dial;       // Display frequency offset (mainly for CW/RTTY)
  float tunestep;   // Default tuning step
  float low;        // Lower edge of IF passband
  float high;       // Upper edge of IF passband
  float gain;       // Gain of filter
};

// Sent in each RTP packet right after header
// NB! because we just copy this into the network stream, it's important that the compiler
// not add any extra padding. To avoid this, the size must be a multiple of 8, the size of the double
struct status {
  double frequency;
  uint32_t samprate;
  uint8_t lna_gain;
  uint8_t mixer_gain;
  uint8_t if_gain;
  uint8_t unused; // pad to 16 bytes
};

// Demodulator state block
#define DATASIZE 65536 // should be a power of 2 for efficiency
struct demod {
  pthread_t input_thread;
  char iq_mcast_address_text[256];
  struct sockaddr input_source_address;
  struct sockaddr ctl_address;
  
  // Front end state
  volatile double samprate;  // True A/D sample rate, assuming same TCXO as tuner
  float min_IF;     // Limits on usable IF due to aliasing, filtering, etc
  float max_IF;
  double calibrate; // Tuner TCXO calibration; - -> frequency low, + -> frequency high
                    // True first LO freq =  (1 + calibrate) * demod.status.frequency

  // status is written by the input thread and read by set_first_LO, etc, so it has to be protected
  pthread_mutex_t status_mutex;
  pthread_cond_t status_cond;
  struct status status; // Last status from FCD
  struct status requested_status; // The status we want the FCD to be in

  float DC_i,DC_q;   // Average DC offsets
  float power_i,power_q; // Average channel powers
  float igain;       // Amplitude gain to be applied to I channel to equalize I & Q, ideally 1
  float sinphi;      // smoothed estimate of I/Q phase error

  int input_fd;      // Raw incoming I/Q data from multicast socket
  int ctl_port;      // Port number for incoming control packets
  int ctl_fd;        // File descriptor for control input socket

  // corr_data is written by the input thread and read by fillbuf in the demod tasks,
  // so it has to be protected by a mutex. The buffer is *NOT* protected from overrun, so ony
  // the reader ever blocks
  pthread_mutex_t data_mutex;
  pthread_cond_t data_cond;
  complex float corr_data[DATASIZE];
  int write_ptr;
  int read_ptr;


  // Per-thread data
  int terminate;
  pthread_t demod_thread;

  double dial_offset; // displayed dial frequency = carrier freq + dial_offset (useful for CW)

  // Second (software) local oscillator parameters
  complex double second_LO_phase;
  double second_LO; // True second LO frequency, including calibration
                    // Same as second_LO_phase step except when sweeping
                    // Provided because round trip through csincos/carg is less accurate
  complex double second_LO_phase_step;  // exp(2*pi*j*second_LO/samprate)

  int frequency_lock;

  // Experimental notch filter
  struct notchfilter *nf;

  // Pre-demod filter parameters
  struct filter *filter;
  int L;            // Signal samples in FFT buffer
  int M;            // Samples in filter impulse response
  int decimate;     // Input/output sample rate decimation ratio
  float low;        // Edges of filter band
  float high;
  float kaiser_beta;

  // Demodulator parameters
  double start_freq;
  enum mode start_mode;
  enum mode mode;   // USB/LSB/FM/etc

  float amplitude;  // Amplitude (not power) of signal after filter
  float noise;      // Minimum amplitude for SNR estimates (experimental)
  float snr;        // Estimated signal-to-noise ratio
  float gain;       // Current audio gain (linear modes only)
  float foffset;    // Frequency offset (FM)
  float pdeviation; // Peak frequency deviation (FM)
  float cphase;     // Carrier phase (DSB/PSK)
  float plfreq;     // PL tone frequency (FM);
  float headroom;   // Audio headroom

  struct audio *audio; // Link to audio output system

  int tunestep;     // User interface cursor location, log10()
};
extern char Libdir[];
extern int Tunestep;
extern struct modetab Modes[];
extern const int Nmodes;

int fillbuf(struct demod *,complex float *,const int);
int setup_input(char const *,char const *);
int setup_output(char const *,char const *);
int LO2_in_range(struct demod *,double f,int);
const double get_freq(struct demod const *);
double set_freq(struct demod *,double,int);
double set_first_LO(struct demod *,double,int);
const double get_first_LO(struct demod const *);
double set_second_LO(struct demod *,double);
const double get_second_LO(struct demod const *);
double set_second_LO_rate(struct demod *,double,int);
int set_filter(struct demod *,float,float);
int set_mode(struct demod *,enum mode);
int set_cal(struct demod *,double);
int spindown(struct demod *,complex float *,int);
void proc_samples(struct demod *,int16_t const *,int);
void activate(struct demod *a,struct demod const *new);

// Save and load (most) receiver state
int savestate(struct demod *,char const *);
int loadstate(struct demod *,char const *);

// Thread entry points
void *display(void *);
void *keyboard(void *);

// Demodulator thread entry points
void *demod_fm(void *);
void *demod_ssb(void *);
void *demod_iq(void *);
void *demod_cam(void *);
void *demod_am(void *);
void *demod_dsb(void *);

#endif
