// $Id: filter.c,v 1.7 2017/06/15 09:28:55 karn Exp karn $
// General purpose filter package using fast convolution (overlap-save)
// and the FFTW3 FFT package
// Generates transfer functions using Kaiser window
// Optional output decimation by integer factor
// Complex input and transfer functions, complex or real output
#include <complex.h>
#include <math.h>
#include <fftw3.h>
#include <stdlib.h>
#include <memory.h>
#include <assert.h>
#include "dsp.h"
#include "filter.h"

// When decimation is used, we assume the filter response drops to negligible
// well below the decimated (lower) Nyquist rate so we can avoid the extra work of adding in
// the aliased frequency components needed to produce exactly the same result as
// decimating the time-domain output by the same ratio

// Real filer is faster type that uses c2r IFFTs to discard imaginary component of output
// Useful for SSB and VSB
// NB: Input and response are still both full-size complex

// response[] must be SIMD-aligned (e.g., with fftw_alloc) and will be freed by delete_filter()
struct filter *create_filter(const int L,const int M, complex float *response,const int decimate,const enum filtertype type){
  struct filter *f;
  const int N = L + M - 1;
  const int N_dec = N / decimate;
  int n;

  f = calloc(1,sizeof(*f));
  f->type = type;
  f->blocksize_in = L;
  f->impulse_length = M;
  f->decimate = decimate;

  // Parameter sanity check
  if((N % decimate) != 0)
    fprintf(stderr,"Warning: FFT size %'u is not divisible by decimation ratio %d\n",N,decimate);

  if((M - 1) % decimate != 0)
    fprintf(stderr,"Warning: Filter length %'u - 1 is not divisible by decimation ratio %d\n",M,decimate);

  f->input_buffer = fftwf_alloc_complex(N);
  memset(f->input_buffer,0,(M-1)*sizeof(*f->input_buffer)); // Clear earlier state
  f->input = f->input_buffer + M - 1;
  f->response = response;
  if(response != NULL && (type == REAL || type == CROSS_CONJ)){
    for(n=0;n<N;n++)
      f->response[n] *= M_SQRT1_2;
  }
  
  f->fdomain = fftwf_alloc_complex(N);
  f->blocksize_out = f->blocksize_in / decimate;
  f->fwd_plan = fftwf_plan_dft_1d(N,f->input_buffer,f->fdomain,FFTW_FORWARD,FFTW_ESTIMATE);
  if(type == REAL){
    f->output_buffer.r = fftwf_alloc_real(N_dec);
    f->output.r = f->output_buffer.r + (M - 1)/decimate;
    f->rev_plan = fftwf_plan_dft_c2r_1d(N_dec,f->fdomain,f->output_buffer.r,FFTW_ESTIMATE);
  } else {
    f->output_buffer.c = fftwf_alloc_complex(N_dec);  
    f->output.c = f->output_buffer.c + (M - 1)/decimate;
    f->rev_plan = fftwf_plan_dft_1d(N_dec,f->fdomain,f->output_buffer.c,FFTW_BACKWARD,FFTW_ESTIMATE);
  }
  return f;
}

int execute_filter(struct filter *f){
  if(f == NULL || f->type == NONE || f->response == NULL)
    return -1;
  const int N = f->blocksize_in + f->impulse_length - 1; // points in input buffer
  const int N_dec = N / f->decimate;                     // points in (decimated) output buffer
  fftwf_execute(f->fwd_plan);  // Forward transform
  // Save for next block - non-destructive copy
  memmove(f->input_buffer,f->input_buffer + f->blocksize_in,(f->impulse_length - 1)*sizeof(*f->input_buffer));

  f->fdomain[0] *= f->response[0];      // DC
  if(f->type == COMPLEX){ // Actually the simplest!
    int n,p,dn;
    for(n=N-1,p=1,dn=N_dec-1; p < N_dec/2; p++,n--,dn--){
      f->fdomain[p]  = f->response[p] * f->fdomain[p]; // Positive frequency
      f->fdomain[dn] = f->response[n] * f->fdomain[n]; // Negative frequency
    }
  } else if(f->type == CROSS_CONJ){
    // hack for ISB; forces negative frequencies onto I, positive onto Q
    int n,p,dn;
    for(n=N-1,p=1,dn=N_dec-1; p < N_dec/2; p++,n--,dn--){
      complex float neg,pos;
      neg = f->response[n] * f->fdomain[n];
      pos = f->response[p] * f->fdomain[p];
      f->fdomain[p] = pos + conjf(neg);
      f->fdomain[dn] = neg - conjf(pos);
    }
  } else if(f->type == REAL){
    // Negative frequencies are assumed by c->r IFFT to be complex conjugate of positive freqs,
    // so we don't need to set them
    int n,p;
    for(n=N-1,p=1; p < N_dec/2; p++,n--)
      f->fdomain[p] = f->response[p] * f->fdomain[p] + conjf(f->response[n] * f->fdomain[n]); // positive freqs
  }
  f->fdomain[N_dec/2] *= f->response[N_dec/2]; // Nyquist frequency
  fftwf_execute(f->rev_plan); // Note: c2r version destroys fdomain[]
  return 0;
}

int delete_filter(struct filter *f){
  if(f != NULL){
    fftwf_destroy_plan(f->fwd_plan);
    fftwf_destroy_plan(f->rev_plan);  
    fftwf_free(f->input_buffer);
    fftwf_free(f->output_buffer.c);
    fftwf_free(f->response);
    fftwf_free(f->fdomain);
    free(f);
  }
  return 0;
}

// Window shape factor for Kaiser window
// Transition region is approx sqrt(1+Beta^2)
float Kaiser_beta = 3.0;


// Hamming window
const float hamming(const int n,const int M){
  const float alpha = 25./46;
  const float beta = (1-alpha);

  return alpha - beta * cos(2*M_PI*n/(M-1));
}

// Hann / "Hanning" window
const float hann(const int n,const int M){
    return 0.5 - 0.5 * cos(2*M_PI*n/(M-1));
}

// Exact Blackman window
const float blackman(const int n,const int M){
  const float a0 = 7938./18608;
  const float a1 = 9240./18608;
  const float a2 = 1430./18608;
  return a0 - a1*cos(2*M_PI*n/(M-1)) + a2*cos(4*M_PI*n/(M-1));
}

// Modified Bessel function of the 0th kind, used by the Kaiser window
static const float i0(const float x){
  float sum = 0;
  float term;
  int k;

  const float t = 0.25 * x * x;
  sum = 1 + t;
  term = t;
  for(k=2;k<40;k++){
    term *= t/(k*k);
    sum += term;
    if(term < 1e-12 * sum)
      break;
  }
  return sum;
}

// Jim Kaiser was in my Bellcore department in the 1980s. Wonder whatever happened to him.
const float kaiser(const int n,const int M, const float beta){
  const float p = 2.0*n/(M-1) - 1;
  return i0(M_PI*beta*sqrtf(1-p*p)) / i0(M_PI*beta);
}

// Apply Kaiser window to filter frequency response
// "response" is SIMD-aligned array of N complex floats
// Impulse response will be limited to first M samples in the time domain
// Phase is adjusted so "time zero" (center of impulse response) is at M/2
int window_filter(const int L,const int M,complex float *response,const float beta){
  fftwf_plan fwd_filter_plan,rev_filter_plan;
  int n;
  complex float *buffer;

  const int N = L + M - 1;
  // fftw_plan can overwrite its buffers, so we're forced to make a temp. Ugh.
  buffer = fftwf_alloc_complex(N);
  fwd_filter_plan = fftwf_plan_dft_1d(N,buffer,buffer,FFTW_FORWARD,FFTW_ESTIMATE);
  rev_filter_plan = fftwf_plan_dft_1d(N,buffer,buffer,FFTW_BACKWARD,FFTW_ESTIMATE);

  // Convert to time domain
  memcpy(buffer,response,N*(sizeof *buffer));
  fftwf_execute(rev_filter_plan);
  fftwf_destroy_plan(rev_filter_plan);
  
  // Shift to beginning of buffer, apply window and scale (N*N)
  const float scale = 1./(N*N);
  for(n = M - 1; n >= 0; n--)
    buffer[n] = buffer[(n-M/2+N)%N] * kaiser(n,M,beta) * scale;

  // Pad with zeroes on right side
  memset(buffer+M,0,(N-M)*sizeof(*buffer));

#if 0
  fprintf(stderr,"Filter impulse response, shifted, windowed and zero padded\n");
  for(n=0;n< N;n++)
    fprintf(stderr,"%d %lg %lg\n",n,crealf(buffer[n]),cimagf(buffer[n]));
#endif
  
  // Now back to frequency domain
  fftwf_execute(fwd_filter_plan);
  fftwf_destroy_plan(fwd_filter_plan);

#if 0
  fprintf(stderr,"Filter response amplitude\n");
  for(n=0;n<N;n++){
    float f = n*192000./N;
    fprintf(stderr,"%.1f %.1f\n",f,power2dB(cnrmf(buffer[n])));
  }
  fprintf(stderr,"\n");
#endif
  memcpy(response,buffer,N*(sizeof *response));
  fftwf_free(buffer);
  return 0;
}
// Real-only counterpart to window_filter()
// response[] is only N/2+1 elements containing DC and positive frequencies only
// Negative frequencies are inplicitly the conjugate of the positive frequencies
int window_rfilter(const int L,const int M,complex float *response,const float beta){
  complex float *buffer;
  float *timebuf;
  fftwf_plan fwd_filter_plan,rev_filter_plan;
  int n;

  const int N = L + M - 1;
  buffer = fftwf_alloc_complex(N/2 + 1); // plan destroys its input
  timebuf = fftwf_alloc_real(N);
  rev_filter_plan = fftwf_plan_dft_c2r_1d(N,buffer,timebuf,FFTW_ESTIMATE);
  fwd_filter_plan = fftwf_plan_dft_r2c_1d(N,timebuf,buffer,FFTW_ESTIMATE);
  
  // Convert to time domain
  memcpy(buffer,response,(N/2+1)*sizeof(*buffer));
  fftwf_execute(rev_filter_plan);
  fftwf_destroy_plan(rev_filter_plan);

  // Shift to beginning of buffer, apply window and scale (N*N)
  const float scale = 1./(N*N);
  for(n = M - 1; n >= 0; n--)
    timebuf[n] = timebuf[(n-M/2+N)%N] * kaiser(n,M,beta) * scale;
  
  // Pad with zeroes on right side
  memset(timebuf+M,0,(N-M)*sizeof(*timebuf));
#if 0
  printf("Filter impulse response, shifted, windowed and zero padded\n");
  for(n=0;n< M;n++)
    printf("%d %lg\n",n,timebuf[n]);
#endif
  
  // Now back to frequency domain
  fftwf_execute(fwd_filter_plan);
  fftwf_destroy_plan(fwd_filter_plan);
  fftwf_free(timebuf);
#if 0
  printf("Filter frequency response\n");
  for(n=0; n < N/2 + 1; n++)
    printf("%d %g %g (%.1f dB)\n",n,crealf(buffer[n]),cimagf(buffer[n]),
	   power2dB(cnrmf(buffer[n])));
#endif
  memcpy(response,buffer,(N/2+1)*sizeof(*response));
  fftwf_free(buffer);
  return 0;
}
