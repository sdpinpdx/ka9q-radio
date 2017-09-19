#define _GNU_SOURCE 1 // allow bind/connect/recvfrom without casting sockaddr_in6
#include <assert.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>
#include <math.h>
#include <complex.h>
#undef I
#include <fftw3.h>

#include "dsp.h"
#include "filter.h"
#include "radio.h"
#include "audio.h"



void *demod_am(void *arg){
  assert(arg != NULL);
  pthread_setname("am");
  struct demod * const demod = arg;
  demod->foffset = 0; // not used
  demod->pdeviation = NAN;

  struct filter * const filter = create_filter(demod->L,demod->M,NULL,demod->decimate,COMPLEX,COMPLEX);
  demod->filter = filter;
  set_filter(filter,demod->samprate/demod->decimate,demod->low,demod->high,demod->kaiser_beta);

  while(!demod->terminate){
    fillbuf(demod,filter->input.c,filter->ilen);
    demod->second_LO_phasor = spindown(demod,filter->input.c); // 2nd LO
    demod->if_power = cpower(filter->input.c,filter->ilen);
    execute_filter(filter);
    //    demod->bb_power = cpower(filter->output.c,filter->olen); // do this below to save time
    if(isnan(demod->n0))
      demod->n0 = compute_n0(demod);
    else
      demod->n0 += .01 * (compute_n0(demod) - demod->n0);

    // Envelope detection
    float average = 0;
    float audio[filter->olen];
    demod->bb_power = 0;
    for(int n=0; n < filter->olen; n++){
      complex float const t = cnrmf(filter->output.c[n]);
      demod->bb_power += t;
      average += audio[n] = sqrtf(t);
    }
    demod->bb_power /= filter->olen;
    average /= filter->olen;

    // AM AGC is carrier-driven
    //    demod->gain = demod->headroom / average;
    demod->gain = 0.5/average;
    // Remove carrier component
    for(int n=0; n<filter->olen; n++)
      audio[n] -= average;

    send_mono_audio(demod->audio,audio,filter->olen,demod->gain);
  }
  delete_filter(filter);
  demod->filter = NULL;
  pthread_exit(NULL);
}
