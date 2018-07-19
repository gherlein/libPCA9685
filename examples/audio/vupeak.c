#include "vupeak.h"

/* Use the newer ALSA API */
#define ALSA_PCM_NEW_HW_PARAMS_API

#include <alsa/asoundlib.h>
#include <PCA9685.h>
#include <signal.h>
#include <fftw3.h>
#include <math.h>
#include <sys/time.h>
#include <unistd.h>
#include "config.h"

// globals, defined here only for intHandler() to cleanup
audiopwm args;
int fd;
char *buffer;
snd_pcm_t *handle;
snd_pcm_t *playhandle;
// verbosity flag
bool verbose = false;
// factor to standardize smoothing time
double speed_scaler;
unsigned int current;
int loop = 0;
bool autoexpand = true;
bool autocontract = true;
FILE* infh;
FILE* inhfh;
FILE* outfh;
FILE* outphfh;
FILE* outplay;
FILE* outwav;


unsigned Microseconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec*1000000 + ts.tv_nsec/1000;
}


void intHandler(int dummy) {
  // turn off all channels
  PCA9685_setAllPWM(fd, args.pwm_addr, _PCA9685_MINVAL, _PCA9685_MINVAL);

  // cleanup alsa
  snd_pcm_drain(handle);
  snd_pcm_close(handle);
  //free(buffer);

  // cleanup fftw
  //fftw_destroy_plan(p);
  //fftw_free(in);
  //fftw_free(out);

  exit(dummy);
}

int initPCA9685(audiopwm args) {
  _PCA9685_DEBUG = args.pwm_debug;
  int afd = PCA9685_openI2C(args.pwm_bus, args.pwm_addr);
  PCA9685_initPWM(afd, args.pwm_addr, args.pwm_freq);
  return afd;
}


snd_pcm_t** initALSA(audiopwm *args, char **bufferPtr) {
  int rc;
  snd_pcm_t* handle;
  snd_pcm_t* playhandle;
  snd_pcm_t** handles = (snd_pcm_t**) malloc(sizeof(snd_pcm_t*) * 2);
  snd_pcm_hw_params_t* params;
  rc = snd_pcm_open(&handle, args->audio_device, SND_PCM_STREAM_CAPTURE, 0);
  if (rc < 0) {
    fprintf(stderr, "unable to open pcm device '%s': %s\n", args->audio_device, snd_strerror(rc));
    exit(1);
  }

  snd_pcm_hw_params_alloca(&params);
  rc = snd_pcm_hw_params_any(handle, params);
  if (rc < 0) fprintf(stderr, "snd_pcm_hw_params_any() failed %d\n", rc);
  rc = snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (rc < 0) fprintf(stderr, "snd_pcm_hw_params_set_access() failed %d\n", rc);
  if (args->audio_bytes == 1) rc = snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S8);
  else if (args->audio_bytes == 2) rc = snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE);
  if (rc < 0) fprintf(stderr, "snd_pcm_hw_params_set_format() failed %d\n", rc);
  rc = snd_pcm_hw_params_set_channels(handle, params, 2);
  if (rc < 0) fprintf(stderr, "snd_pcm_hw_params_set_channels() failed %d\n", rc);

  rc = snd_pcm_hw_params_set_rate_near(handle, params, &args->audio_rate, NULL);
  if (rc < 0) fprintf(stderr, "snd_pcm_hw_params_set_rate_near() failed %d\n", rc);
  fprintf(stdout, "sample rate %u\n", args->audio_rate);

  rc = snd_pcm_hw_params_set_period_size_near(handle, params, (snd_pcm_uframes_t *) &args->audio_period, NULL);
  if (rc < 0) fprintf(stderr, "snd_pcm_hw_params_set_period_size_near() failed %d\n", rc);
  fprintf(stdout, "audio period %d\n", args->audio_period);
  args->audio_buffer_period = args->audio_period;

  if (args->audio_overlap) args->audio_buffer_period = args->audio_period << args->audio_overlap;
  fprintf(stdout, "buffer period %d\n", args->audio_buffer_period);

  rc = snd_pcm_hw_params(handle, params);
  if (rc < 0) {
    fprintf(stderr, "unable to set hw parameters: (%d), %s\n", rc, snd_strerror(rc));
    exit(1);
  }
  args->audio_buffer_size = args->audio_buffer_period * args->audio_bytes * args->audio_channels; /* 2 bytes/sample, 2 channels */
  fprintf(stdout, "buffer size %d\n", args->audio_buffer_size);
  *bufferPtr = (char *) malloc(args->audio_buffer_size);

  if (args->vocoder) {
    rc = snd_pcm_open(&playhandle, args->audio_playback_device, SND_PCM_STREAM_PLAYBACK, 0);
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(playhandle, params);
    snd_pcm_hw_params_set_access(playhandle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (args->audio_bytes == 1) snd_pcm_hw_params_set_format(playhandle, params, SND_PCM_FORMAT_S8);
    else if (args->audio_bytes == 2) snd_pcm_hw_params_set_format(playhandle, params, SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(playhandle, params, args->audio_channels);
    unsigned int val = args->audio_rate;
    int dir;
    snd_pcm_hw_params_set_rate_near(playhandle, params, &val, &dir);
    fprintf(stderr, "playback sampling rate %d\n", val);
    long unsigned int frames = args->audio_period;
    snd_pcm_hw_params_set_period_size_near(playhandle, params, &frames, &dir);
    fprintf(stderr, "playback period size %lu\n", frames);
    rc = snd_pcm_hw_params(playhandle, params);
    if (rc < 0) {
      fprintf(stderr, "unable to set hw parameters: %s\n", snd_strerror(rc));
      exit(1);
    } // if rc
    snd_pcm_sw_params_t* swparams;
    snd_pcm_sw_params_alloca(&swparams);
    snd_pcm_sw_params_current(handle, swparams);
    rc = snd_pcm_sw_params_set_start_threshold(handle, swparams, (snd_pcm_uframes_t) args->audio_buffer_size);
    //rc = snd_pcm_sw_params_set_avail_min(handle, swparams, frames);
    if (rc < 0) {
      fprintf(stderr, "unable to set sw start threshold: %s\n", snd_strerror(rc));
      exit(1);
    }
    rc = snd_pcm_sw_params(handle, swparams);
    if (rc < 0) {
      fprintf(stderr, "unable to set sw parameters: %s\n", snd_strerror(rc));
      exit(1);
    }
  } // if vocoder
  handles[0] = handle;
  handles[1] = playhandle;
  return handles;
}



double *hanning(int N) {
  int i;
  double *window = (double *) malloc(sizeof(double) * N);
  int M = N % 2 == 0 ? N / 2 : (N + 1) / 2;
  if (verbose) fprintf(stdout, "hanning %d %d\n", N, M);
  for (i = 0; i < M; i++) {
    window[i] = 0.5 * (1 - cos(2 * M_PI * i / (N - 1)));
    window[N - i - 1] = window[i];
    if (verbose) printf("%d: %f  %d: %f\n", i, window[i], N - i - 1, window[N - i - 1]);
  }
  if (verbose) {
    for (i = 0; i < N; i++) {
      fprintf(stdout, "%f ", window[i]);
    }
    fprintf(stdout, "\n");
  }
  return window;
}


void process_args(int argc, char **argv) {
  char *usage = "\
Usage: vupeak [-m level|spectrum] [-d audio device] [-p audio period]\n\
              [-r audio rate] [-c audio channels] [-o audio overlap] [-B audio bytes] [-P audio playback device]\n\
              [-b pwm bus] [-a pwm address] [-f pwm frequency]\n\
              [-v] [-D] [-s pwm smoothing] [-h] [-t] [-w] [-V]\n\
where\n\
  -m sets the mode of audio processing (spectrum)\n\
  -d sets the audio device (default)\n\
  -P sets the audio playback device (default)\n\
  -p sets the audio period (256)\n\
  -r sets the audio rate (44100)\n\
  -c sets the audio channels (2)\n\
  -o sets the audio overlap (2)\n\
  -B sets the audio bytes (2)\n\
  -b sets the pwm i2c bus (1)\n\
  -a sets the pwm i2c address (0x40)\n\
  -f sets the pwm frequency (200)\n\
  -v sets verbose to true (false)\n\
  -D sets debug to true (false)\n\
  -s sets the pwm smoothing (1)\n\
  -h sets the fft hanning to true (false)\n\
  -t sets the test period to true (false)\n\
  -w sets the ascii waterfall to true (false)\n\
  -V sets the vocoder to true (false)\n\
";

  // default values
  args.mode = 2;
  args.audio_device = "default";
  args.audio_playback_device = "default";
  args.audio_period = 256;
  args.audio_rate = 44100;
  args.audio_channels = 2;
  args.audio_overlap = 2;
  args.audio_bytes = 2;
  args.pwm_bus = 1;
  args.pwm_addr = 0x40;
  args.pwm_freq = 200;
  args.pwm_debug = false;
  args.pwm_smoothing = 1;
  args.fft_hanning = false;
  args.test_period = false;
  args.ascii_waterfall = false;
  args.vocoder = false;

  opterr = 0;
  int c;
  while ((c = getopt(argc, argv, "m:d:p:r:c:o:b:a:f:vDs:htwV")) != -1) {
    switch (c) {
      case 'm':
        args.mode = 0;
        if (strcmp(optarg, "level") == 0) args.mode = 1;
        else if (strcmp(optarg, "spectrum") == 0) args.mode = 2;
        if (args.mode == 0) {
          fprintf(stderr, "Illegal mode %s\n", optarg);
          exit(-1);
        } // if
        break;
      case 'd':
        args.audio_device = optarg;
        break;
      case 'P':
        args.audio_playback_device = optarg;
        break;
      case 'p':
        args.audio_period = atoi(optarg);
        break;
      case 'r':
        args.audio_rate = atoi(optarg);
        break;
      case 'c':
        args.audio_channels = atoi(optarg);
        break;
      case 'o':
        args.audio_overlap = atoi(optarg);
        break;
      case 'B':
        args.audio_bytes = atoi(optarg);
        break;
      case 'b':
        args.pwm_bus = atoi(optarg);
        break;
      case 'a':
        args.pwm_addr = atoi(optarg);
        break;
      case 'f':
        args.pwm_freq = atoi(optarg);
        break;
      case 'v':
        verbose = true;
        break;
      case 'D':
        args.pwm_debug = true;
        break;
      case 's':
        args.pwm_smoothing = atoi(optarg);
        break;
      case 'h':
        args.fft_hanning = true;
        break;
      case 't':
        args.test_period = true;
        break;
      case 'w':
        args.ascii_waterfall = true;
        break;
      case 'V':
        args.vocoder = true;
        break;
      case '?':
        fprintf(stdout, "%s", usage);
        exit(0);
      default:
        fprintf(stderr, "optopt '%c'\n", optopt);
        fprintf(stderr, "%s", usage);
        abort();
    } //switch
  } // while

  // sanity checks
  if (args.mode == 1) {
    fprintf(stdout, "'level' mode suggested args: -o 0 -p 256 -r 192000\n");
  } // if mode
} // process_args


char *representation(float val) {
  if (val < 71) return " ";
  if (val < 72) return "-";
  if (val < 73) return ".";
  if (val < 74) return ",";
  if (val < 75) return ":";
  if (val < 76) return ";";
  if (val < 77) return "+";
  if (val < 78) return "=";
  if (val < 79) return "&";
  if (val < 80) return "@";
  return "# ";
}


void level(char* buffer) {
  static int minValue = 32000;
  static int maxValue = -32000;
  static int average = 0;
  // find the min and max value
  int sample;
  long tmp;
  int maxSample=-32000;
  int minSample=32000;

  // process all frames recorded
  unsigned int frame;
  for (frame = 0; frame < args.audio_buffer_period; frame++) {
    tmp = (long) ((char*) buffer)[2 * args.audio_channels * frame + 1] << 8 | ((char*) buffer)[2 * args.audio_channels * frame];
    if (tmp < 32768) sample = tmp;
    else sample = tmp - 65536;
    if (sample > maxSample) {
      maxSample = sample;
    } // if sample >
    if (sample < minSample) {
      minSample = sample;
    } // if sample <
  } // for frame

  // intensity-based value
  int intensity_value = maxSample - minSample;

  // minValue is the smallest value seen, use for offset
  if (intensity_value < minValue) {
    minValue = intensity_value;
  }
  if (intensity_value > maxValue) {
    maxValue = intensity_value;
  }
  intensity_value -= minValue;

  // modified moving average for smoothing
  int alpha = args.pwm_smoothing;
  alpha *= speed_scaler;
  average = (intensity_value + (alpha-1) * average) / alpha;

  int ratio = 100.0 * average / (maxValue - minValue);
  ratio = (ratio / 10.0) * (ratio / 10.0);
  ratio = (ratio / 10.0) * (ratio / 10.0);
  if (ratio < 0) ratio = 0;
  if (ratio > 100) ratio = 100;

  int display = ratio / 100.0 * _PCA9685_MAXVAL;
  if (verbose) fprintf(stdout, "%d %d\n", intensity_value, ratio);

  // update the pwms
  PCA9685_setAllPWM(fd, args.pwm_addr, 0, display);
} // level


void insert_sample(char* buffer, int frame, long value) {
  if (args.audio_bytes == 1 && value < 0) value += 256;
  if (args.audio_bytes == 2 && value < 0) value += 65536;
  value /= args.audio_buffer_period;
  buffer[args.audio_bytes * args.audio_channels * frame] = value % 256;
  if (args.audio_bytes == 2) buffer[args.audio_bytes * args.audio_channels * frame + 1] = value / 256;
  // zero out other channel
  if (args.audio_channels == 2) {
    buffer[args.audio_bytes * args.audio_channels * frame + args.audio_bytes] = 0;
    if (args.audio_bytes == 2) buffer[args.audio_bytes * args.audio_channels * frame + args.audio_bytes + 1] = 0;
  } // if 2 channels
} // insert sample


double extract_sample(char* buffer, int frame) {
  // extract a signed int from the unsigned 2-byte first channel of the frame
  long tmp = (long) ((char*) buffer)[args.audio_bytes * args.audio_channels * frame];
  if (args.audio_bytes == 2) tmp |= (long) ((char*) buffer)[args.audio_bytes * args.audio_channels * frame + 1] << 8;
  if (args.audio_bytes == 1 && tmp >= 128) tmp = tmp - 256;
  else if (args.audio_bytes == 2 && tmp >= 32768) tmp = tmp - 65536;
  // insert the sample into the complex input array, zeroing the imaginary part
  return (double) tmp;
} // extract sample


void vocoder(fftw_complex* in, fftw_plan pi) {
  int outsize = args.audio_period * args.audio_channels * args.audio_bytes;
  //printf("outsize %d\n", outsize);
  char* outbuffer = (char*) malloc(outsize);
  fftw_execute(pi);
  for (unsigned int i = 0; i < args.audio_period; i++) {
    //printf("%d:%d ", i, in[i][0]);
    insert_sample(outbuffer, i, in[i][0]);
  } // for i
  //printf("\n");
  if (args.test_period) {
    //for (unsigned int i = 0; i < args.audio_period * args.audio_channels * args.audio_bytes; i++) {
    for (unsigned int i = 0; i < args.audio_period; i++) {
      fprintf(outplay, "%f ", in[i][0]);
      fprintf(outwav, "%f ", extract_sample(outbuffer, i));
      if (i == args.audio_period - 1) {
        fprintf(outplay, "\n");
        fprintf(outwav, "\n");
      } // if i
    } // for i
  } // if test period
  int rc = snd_pcm_writei(playhandle, outbuffer, outsize);
  if (rc == -EPIPE) {
    // EPIPE means underrun
    fprintf(stderr, "underrun occurred\n");
    snd_pcm_prepare(playhandle);
  } else if (rc < 0) {
    fprintf(stderr, "error from writei: %s\n", snd_strerror(rc));
  } else if ((unsigned int) rc != outsize) {
    fprintf(stderr, "short write, write %d frames\n", rc);
  }
} // vocoder


#define MAX_LENGTH 10000

void unwrap(double p[], int N) {
    double dp[MAX_LENGTH];     
    double dps[MAX_LENGTH];    
    double dp_corr[MAX_LENGTH];
    double cumsum[MAX_LENGTH];
    double cutoff = M_PI;               /* default value in matlab */
    int j;

    assert(N <= MAX_LENGTH);
   // incremental phase variation 
   // MATLAB: dp = diff(p, 1, 1);
    for (j = 0; j < N-1; j++)
      dp[j] = p[j+1] - p[j];
      
   // equivalent phase variation in [-pi, pi]
   // MATLAB: dps = mod(dp+dp,2*pi) - pi;
    for (j = 0; j < N-1; j++)
      dps[j] = (dp[j]+M_PI) - floor((dp[j]+M_PI) / (2*M_PI))*(2*M_PI) - M_PI;

   // preserve variation sign for +pi vs. -pi
   // MATLAB: dps(dps==pi & dp>0,:) = pi;
    for (j = 0; j < N-1; j++)
      if ((dps[j] == -M_PI) && (dp[j] > 0))
        dps[j] = M_PI;

   // incremental phase correction
   // MATLAB: dp_corr = dps - dp;
    for (j = 0; j < N-1; j++)
      dp_corr[j] = dps[j] - dp[j];
      
   // Ignore correction when incremental variation is smaller than cutoff
   // MATLAB: dp_corr(abs(dp)<cutoff,:) = 0;
    for (j = 0; j < N-1; j++)
      if (fabs(dp[j]) < cutoff)
        dp_corr[j] = 0;

   // Find cumulative sum of deltas
   // MATLAB: cumsum = cumsum(dp_corr, 1);
    cumsum[0] = dp_corr[0];
    for (j = 1; j < N-1; j++)
      cumsum[j] = cumsum[j-1] + dp_corr[j];

   // Integrate corrections and add to P to produce smoothed phase values
   // MATLAB: p(2:m,:) = p(2:m,:) + cumsum(dp_corr,1);
    for (j = 1; j < N; j++)
      p[j] += cumsum[j-1];
}


void spectrum(char* buffer, double* han, fftw_plan p, fftw_plan pi, fftw_complex* in, fftw_complex* out) {
  static int prevwater;
  static int prevstats;
  unsigned int bins[16] = {0,0,1, 0,2,2, 0,3,0, 4,4,0, 20,0,0};
  unsigned int binwidths[16] = {0,0,1, 0,1,1, 0,1,0, 16,16,0, 20,0,0};
  static double* mins = NULL;
  static double* maxs = NULL;
  if (mins == NULL) {
    mins = (double*) malloc(sizeof(double) * 16);
    maxs = (double*) malloc(sizeof(double) * 16);
    int i;
    for (i = 0; i < 16; i++) {
      if (autoexpand) {
        mins[i] = 100;  maxs[i] = 0;
      } else {
        mins[i] = 70;  maxs[i] = 90;
        unsigned int min;
        unsigned int max;
        switch(i) {
          case 0: min = 38; max = 92; break;
          case 1: min = 38; max = 92; break;
          case 2: min = 38; max = 92; break;
          case 3: min = 30; max = 88; break;
          case 4: min = 30; max = 88; break;
          case 5: min = 30; max = 88; break;
          case 6: min = 80; max = 83; break;
          case 7: min = 80; max = 83; break;
          case 8: min = 80; max = 83; break;
          case 9: min = 60; max = 86; break;
          case 10: min = 60; max = 86; break;
          case 11: min = 60; max = 86; break;
          case 12: min = 74; max = 76; break;
          case 13: min = 74; max = 76; break;
          case 14: min = 74; max = 76; break;
          case 15: min = 0; max = 0; break;
        } // switch
        mins[i] = min;
        maxs[i] = max;
      } // else not autoexpand
    } // for i
  } // mins is NULL

  // window for fftw
  unsigned int frame;
  double sample;
  for (frame = 0; frame < args.audio_buffer_period; frame++) {
    sample = extract_sample(buffer, frame);
    in[frame][0] = (double) sample;
    in[frame][1] = 0;
    // for testing, save the waveform
    if (args.test_period) {
      fprintf(infh, "%f\n", in[frame][0]);
    } // if test period
  } // for frame

  // apply hanning window
  if (args.fft_hanning) {
    unsigned int i;
    for (i = 0; i < args.audio_buffer_period; i++) {
      in[i][0] *= han[i];
      // for testing, save the windowed waveform
      if (args.test_period) {
        fprintf(inhfh, "%f ", in[i][0]);
        if (i == args.audio_buffer_period - 1) fprintf(infh, "\n");
      } // if test period
    } // for i
  } // if hanning

  // fftw
  fftw_execute(p);

  // for testing, save transform output
  if (args.test_period) {
    unsigned int i;
    double phases[args.audio_buffer_period];
    for (i = 0; i < args.audio_buffer_period; i++) {
      double absval = 20.0 * log10f(2.0 * sqrtf(out[i][0]*out[i][0] + out[i][1]*out[i][1]) / args.audio_buffer_period);
      fprintf(outfh, "%f\t", absval);
      double phase = atan(out[i][1]/out[i][0]);
      phases[i] = phase;
      //fprintf(outphfh, "%f\t", phase);
    } // for i
    unwrap(phases, args.audio_buffer_period);
    for (i = 0; i < args.audio_buffer_period; i++) {
      fprintf(outphfh, "%f\t", phases[i]);
    } // for i
    fprintf(outphfh, "\n");
    fprintf(outfh, "\n");
  } // if test period

  if (args.ascii_waterfall && current - prevwater > 50000) {
    int i;
    for (i = 1; i < 40; i++) {
      double val = 20.0 * log10f(2.0 * sqrtf(out[i][0]*out[i][0] + out[i][1]*out[i][1]) / args.audio_buffer_period);
      printf("%s", representation(val));
    } // for i
    printf("\n");
    prevwater = current;
  } // if waterfall

  static unsigned int pwmoff[16];
  int pwmindex;
  for (pwmindex = 0; pwmindex < 16; pwmindex++) {
    unsigned int binindex = bins[pwmindex];
    unsigned int width = binwidths[pwmindex];
    if (binindex == 0) {
      pwmoff[pwmindex] = 0;
      continue;
    } // if index

    // determine the 'amp' for each pwm index
    unsigned int j;
    double amp = 0;
    int ampbinindex = binindex;
    for (j = 0; j < width; j++) {
    // normalize by the number of frames in a period and the hanning factor
      double mag = 2.0 * sqrtf(out[binindex + j][0] * out[binindex + j][0] + out[binindex + j][1] * out[binindex + j][1]) / args.audio_buffer_period;
      double thisamp = 20 * log10f(mag);
      if (thisamp > amp) {
        amp = thisamp;
        ampbinindex = binindex + j;
      } // if thisamp
    } // for j

    if (autoexpand) {
      int a = 3;
      a *= speed_scaler;
      if (verbose) printf("check: amp %f mins[%d] %f maxs[%d] %f\n", amp, pwmindex, mins[pwmindex], pwmindex, maxs[pwmindex]);
      if (amp > maxs[pwmindex]) {
        maxs[pwmindex] = ((a - 1) * maxs[pwmindex] + amp) / a;
        if (verbose) printf("maxs[%d] %f amp %f newmax %f\n", pwmindex, maxs[pwmindex], amp, ((a - 1) * maxs[pwmindex] + amp) / a);
      }
      if (amp < mins[pwmindex]) {
        if (verbose) printf("mins[%d] %f amp %f newmin %f\n", pwmindex, mins[pwmindex], amp, ((a - 1) * mins[pwmindex] + amp) / a);
        mins[pwmindex] = ((a - 1) * mins[pwmindex] + amp) / a;
      }
  
      int minmin = 15;
      if (binindex == 1) minmin = 38;
      if (binindex == 2) minmin = 37;
      if (mins[pwmindex] < minmin) {
        mins[pwmindex] = minmin;
      } // if minmax
      int minmax = minmin + 15;
      if (maxs[pwmindex] < minmax) {
        maxs[pwmindex] = minmax;
      } // if minmax
    } // if autoexpand

    if (verbose) fprintf(stdout, "%2d:%3d %f-%f -> ", ampbinindex, (int) amp, mins[pwmindex], maxs[pwmindex]);
    if (amp > mins[pwmindex]) {
      double ratio = (amp - mins[pwmindex]) / (maxs[pwmindex] - mins[pwmindex]);
      if (ratio > 1.0) ratio = 1.0;
      if (ratio < 0.0) ratio = 0.0;
      ratio *= ratio;
      ratio *= ratio;
      ratio *= ratio;
      ratio *= ratio;
      ratio *= ratio;
      if (verbose) printf(" %0.2f ", ratio);
      double val = _PCA9685_MAXVAL * ratio;
      if (val > 4096) val = 4096;
      int alpha = args.pwm_smoothing;
      alpha *= speed_scaler;
      double scaled = val < pwmoff[pwmindex] ? ((alpha - 1) * pwmoff[pwmindex] + val) / alpha : val;
      pwmoff[pwmindex] = (unsigned int) scaled;
    } else {
      pwmoff[pwmindex] = 0;
    } // if amp
    if (verbose) fprintf(stdout, "%u\n", pwmoff[pwmindex]);
  } // for i
  // update the pwms
  unsigned int pwmon[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  PCA9685_setPWMVals(fd, args.pwm_addr, pwmon, pwmoff);

  if (autocontract) {
    { int pwmindex;
      for (pwmindex = 0; pwmindex < 16; pwmindex++) {
        mins[pwmindex] += 0.01 / speed_scaler;
        maxs[pwmindex] -= 0.1 / speed_scaler;
      } // for pwmindex
    }
  } // if autocontract
/*
  if (current - prevstats > 3000000) {
    unsigned int diff = current - prevstats;
    double ms = (double) diff / 1000.0 / loop;
    printf("%d %0.2f %0.2f  ", loop, ms, 1000.0/ms);
    int j;
    for (j = 0; j < 16; j++) {
      if (bins[j] == 0) continue;
      printf("%3.0f-%-3.0f ", mins[j], maxs[j]);
    } // for j
    printf("\n");
    prevstats = current;
    loop = 0;
  }
*/
  if (args.vocoder) {
    vocoder(in, pi);
  } // if vocoder
} // spectrum


int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  fprintf(stdout, "vupeak %d.%d\n", libPCA9685_VERSION_MAJOR, libPCA9685_VERSION_MINOR);
  process_args(argc, argv);

  signal(SIGINT, intHandler);

  // ALSA init
  snd_pcm_t** handles = initALSA(&args, &buffer);
  handle = handles[0];
  playhandle = handles[1];

  // libPCA9685 init
  fd = initPCA9685(args);

  // fftw init
  // the buffer period may be bigger than the audio period
  int N = args.audio_buffer_period;
  printf("N %d\n", N);
  fftw_plan p, pi;
  fftw_complex* in = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * N);
  fftw_complex* out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * N);
  p = fftw_plan_dft_1d(N, in, out, FFTW_FORWARD, FFTW_ESTIMATE);
  pi = fftw_plan_dft_1d(N, in, out, FFTW_BACKWARD, FFTW_ESTIMATE);
  double *han;
  if (args.fft_hanning) han = hanning(N);

  int rc = 64;
  rc = 1024;

  printf("loops  ms    hz\n");

  unsigned int recorded_periods = 0;
  bool initialized = false;
  speed_scaler = 1024.0 / args.audio_period;
  if (args.test_period) {
    infh = fopen("input.dat", "w");
    inhfh = fopen("inputwin.dat", "w");
    outfh = fopen("outputspec.dat", "w");
    outphfh = fopen("outputphase.dat", "w");
    outplay = fopen("output.dat", "w");
    outwav = fopen("outputwav.dat", "w");
  } // if test period
  while (1) {
    current = Microseconds();
    if (args.audio_overlap && recorded_periods > 0) {
      // shift prior data to the left, record into buffer one hop from the end
      // move buffer + one hop to buffer
      char* dst = buffer;
      char* src = buffer + args.audio_bytes * args.audio_channels * args.audio_period;
      int length = args.audio_buffer_size - args.audio_bytes * args.audio_channels * args.audio_period;
      memmove(dst, src, length);
    } // overlap
    char* onehopfromend = buffer + args.audio_buffer_size - args.audio_bytes * args.audio_channels * args.audio_period;
    rc = snd_pcm_readi(handle, onehopfromend, args.audio_period);
    if (rc == -EPIPE) {
      fprintf(stderr, "overrun occurred\n");
      snd_pcm_prepare(handle);
    } else if (rc < 0) {
      fprintf(stderr, "error from read: %s\n", snd_strerror(rc));
    } else if (rc != (int) args.audio_period) {
      fprintf(stderr, "short read, read %d frames\n", rc);
    } else {
      if (!initialized && ++recorded_periods < args.audio_buffer_period / args.audio_period) {
        printf("buffering one period\n");
        continue;
      }
      if (!initialized) {
        initialized = true;
        printf("done buffering\n");
        printf("periods %d buffer period %d audio period %d buffer period / audio period %d\n", recorded_periods, args.audio_buffer_period, args.audio_period, args.audio_buffer_period / args.audio_period);
      }

      if (args.mode == 1) {
        level(buffer);
      } // if mode 1

      else if (args.mode == 2) {
        spectrum(buffer, han, p, pi, in, out);
      } // if mode 2
    } // else good audio read

    loop++;
    // for testing, to process only one period
    //if (args.test_period) exit(0);
  } // while 1
} // main
