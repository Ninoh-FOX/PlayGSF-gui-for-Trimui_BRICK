#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <libgen.h>
#include <chrono>
#include <assert.h>
#include <algorithm>
#include <alsa/asoundlib.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <mutex>
#include <string>
#include <math.h>

using std::chrono::steady_clock;
using std::chrono::duration;
#include "types.h"

extern "C" {
#include "VBA/psftag.h"
#include "gsf.h"
}

extern "C" {
int defvolume=1000;
int relvolume=1000;
int TrackLength=0;
int FadeLength=0;
int IgnoreTrackLength, DefaultLength=150000;
int playforever=0;
int fileoutput=0;
int TrailingSilence=1000;
int DetectSilence=0, silencedetected=0, silencelength=5;
int noinfo=0;
}
std::string OutputFile = std::string("");
int cpupercent=0, sndSamplesPerSec, sndNumChannels;
int sndBitsPerSample=16;
int bass_boost_enabled = 0;

int deflen=120,deffade=10;
#define W 800
int draw_buf[2][6][2*W];
int n_old[2][6];
// Draw buf starts full, all samples are 0
int last[2][6] = {
	{2*W, 2*W, 2*W, 2*W, 2*W, 2*W},
	{2*W, 2*W, 2*W, 2*W, 2*W, 2*W},
};

// Coeficientes filtro low-shelf
static float b0_ls, b1_ls, b2_ls, a1_ls, a2_ls;

// Estados del filtro para canal L y R
static float x1L=0, x2L=0, y1L=0, y2L=0;
static float x1R=0, x2R=0, y1R=0, y2R=0;

int curr_buf;
std::mutex bufmtx;

extern unsigned short soundFinalWave[1470];
extern int soundBufferLen;
extern int soundIndex;
extern int8_t soundBuffer[4][735];
extern uint8_t *ioMem;
#ifdef NO_INTERPOLATION
int16_t directBuffer[2][735];
#else
extern int16_t directBuffer[2][735];
#endif
extern int soundLevel1;
extern int enableDS;

extern char soundEcho;
extern char soundLowPass;
extern char soundReverse;
extern char soundQuality;

double decode_pos_ms; // current decoding position, in milliseconds
int seek_needed; // if != -1, it is the point that the decode thread should seek to, in ms.

static int g_playing = 0;
static int g_must_exit = 0;

static snd_pcm_t *pcm_handle;
static snd_pcm_hw_params_t *hw_params;
static snd_pcm_uframes_t frames;

extern "C" int LengthFromString(const char * timestring);
extern "C" int VolumeFromString(const char * volumestring);

extern "C" void end_of_track()
{
	g_playing = 0;
}

// Declaración global para conservar el estado del filtro
static float prev_filtered[2][6][2*W] = {{{0}}}; // Buffer para almacenar muestras filtradas previas

template<typename T>
void updateBuf(int c, int ch, float m, T *data, int datalen) {
    int zeroCrossing = -1;
    int min = *std::min_element(draw_buf[c][ch], draw_buf[c][ch] + W);
    int max = *std::max_element(draw_buf[c][ch], draw_buf[c][ch] + W);
    int th = (max + min) / 2;

    int min_need = W - soundIndex;
    int search_head = last[c][ch] - min_need;

    // Buscar cruce por cero para sincronizar el buffer
    for (int i = search_head; i >= 1; i--) {
        if (draw_buf[c][ch][i - 1] >= th && draw_buf[c][ch][i] < th) {
            zeroCrossing = i;
            break;
        }
    }

    if (zeroCrossing < n_old[c][ch])
        zeroCrossing = search_head;

    n_old[!c][ch] = last[c][ch] - zeroCrossing;
    memcpy(draw_buf[!c][ch], draw_buf[c][ch] + zeroCrossing, sizeof(int) * n_old[!c][ch]);

    const float alpha = 0.1f; // Factor de suavizado para el filtro paso bajo

    // Aplicar filtro paso bajo exponencial (media móvil ponderada)
    for (int i = 0; i < datalen; i++) {
        float raw_sample = data[i] * m;
        float filtered_sample;
        if (i == 0)
            filtered_sample = alpha * raw_sample + (1.0f - alpha) * prev_filtered[c][ch][n_old[!c][ch] - 1];
        else
            filtered_sample = alpha * raw_sample + (1.0f - alpha) * prev_filtered[c][ch][n_old[!c][ch] + i - 1];

        prev_filtered[c][ch][n_old[!c][ch] + i] = filtered_sample;
        draw_buf[!c][ch][n_old[!c][ch] + i] = static_cast<int>(filtered_sample);
    }

    last[!c][ch] = n_old[!c][ch] + datalen;
    assert(last[!c][ch] >= W);
}
static void lowshelf_init(float fs, float f0, float gainDB) {
    float A  = powf(10.0f, gainDB / 40.0f);
    float w0 = 2.0f * M_PI * f0 / fs;
    float alpha = sinf(w0) / 2.0f * sqrtf( (A + 1/A) * (1.0f/0.707f - 1.0f) + 2.0f );
    float k = cosf(w0);

    float b0f =    A*((A+1) - (A-1)*k + 2.0f*sqrtf(A)*alpha);
    float b1f =  2*A*((A-1) - (A+1)*k);
    float b2f =    A*((A+1) - (A-1)*k - 2.0f*sqrtf(A)*alpha);
    float a0f =       (A+1) + (A-1)*k + 2.0f*sqrtf(A)*alpha;
    float a1f =  -2*((A-1) + (A+1)*k);
    float a2f =       (A+1) + (A-1)*k - 2.0f*sqrtf(A)*alpha;

    b0_ls = b0f / a0f;
    b1_ls = b1f / a0f;
    b2_ls = b2f / a0f;
    a1_ls = a1f / a0f;
    a2_ls = a2f / a0f;
}
static void lowshelf_process(short *samples, int count) {
    for (int i = 0; i < count; i += 2) {
        float inL = samples[i];
        float inR = samples[i+1];

        float outL = b0_ls*inL + b1_ls*x1L + b2_ls*x2L - a1_ls*y1L - a2_ls*y2L;
        float outR = b0_ls*inR + b1_ls*x1R + b2_ls*x2R - a1_ls*y1R - a2_ls*y2R;

        x2L = x1L; x1L = inL; y2L = y1L; y1L = outL;
        x2R = x1R; x1R = inR; y2R = y1R; y1R = outR;

        if (outL > 32767.0f) outL = 32767.0f;
        if (outL < -32768.0f) outL = -32768.0f;
        if (outR > 32767.0f) outR = 32767.0f;
        if (outR < -32768.0f) outR = -32768.0f;

        samples[i]   = (short)outL;
        samples[i+1] = (short)outR;
    }
}
extern "C" void writeSound(void)
{
    int ret = soundBufferLen;

    int ratio = ioMem[0x82] & 3;
    int dsaRatio = ioMem[0x82] & 4;
    int dsbRatio = ioMem[0x82] & 8;
    float m = soundLevel1;

    switch(ratio) {
        case 0:
        case 3:
            m /= 4.0;
            break;
        case 1:
            m /= 2.0;
            break;
        case 2:
            break;
    }

    for (int i = 0; i < 4; i++)
        updateBuf(curr_buf, i, m, soundBuffer[i], soundIndex);

    if (!dsaRatio) m = 0.5; else m = 1;
    m = m / float(soundLevel1) / 52.0;
    updateBuf(curr_buf, 4, m, directBuffer[0], soundIndex);

    if (!dsbRatio) m = 0.5; else m = 1;
    m = m / float(soundLevel1) / 52.0;
    updateBuf(curr_buf, 5, m, directBuffer[1], soundIndex);

    bufmtx.lock();
    curr_buf = !curr_buf;
    bufmtx.unlock();

    static short tempBuffer[1470];
    memcpy(tempBuffer, soundFinalWave, ret);

    snd_pcm_sframes_t delay_frames = 0;
    if (snd_pcm_delay(pcm_handle, &delay_frames) < 0)
        delay_frames = 0;

    int time_to_end_ms = TrackLength - FadeLength;
    if (time_to_end_ms < 0) time_to_end_ms = 0;

    if (time_to_end_ms <= FadeLength) {
        float factor = (float)time_to_end_ms / (float)FadeLength;
        if (factor < 0.0f) factor = 0.0f;

        int samplesCount = ret / sizeof(short);
        for (int i = 0; i < samplesCount; i++) {
            tempBuffer[i] = (short)(tempBuffer[i] * factor);
        }
    }

    int frames_to_deliver = ret / (2 * sndNumChannels);
	if (bass_boost_enabled) {
        int samplesCount = ret / sizeof(short);
        lowshelf_process(tempBuffer, samplesCount);
    }  
    int written = snd_pcm_writei(pcm_handle, tempBuffer, frames_to_deliver);
    if (written < 0) {
        snd_pcm_prepare(pcm_handle);
    }

    decode_pos_ms += (ret / (2 * sndNumChannels)) * 1000.0 / sndSamplesPerSec;
}

extern "C" void signal_handler(int sig)
{
	struct timeval tv_now;
	int elaps_milli;

	static int first=1;
	static struct timeval last_int = {0,0};

	g_playing = 0;
	;gettimeofday(&tv_now, NULL);

	if (first) {
		first = 0;
	}
	else {
		elaps_milli = (tv_now.tv_sec - last_int.tv_sec)*1000;
		elaps_milli += (tv_now.tv_usec - last_int.tv_usec)/1000;

		if (elaps_milli < 1500) {
			g_must_exit = 1;
		}
	}
	memcpy(&last_int, &tv_now, sizeof(struct timeval));
}

static void shuffle_list(char *filelist[], int num_files)
{
	int i, n;
	char *tmp;
	srand((int)time(NULL));
	for (i=0; i<num_files; i++)
	{
		tmp = filelist[i];
		n = (int)((double)num_files*rand()/(RAND_MAX+1.0));
		filelist[i] = filelist[n];
		filelist[n] = tmp;
	}
}

extern "C" void handle_bass_toggle(int sig) {
    if (sig == SIGUSR2) {
        bass_boost_enabled = !bass_boost_enabled;
        fprintf(stderr, "BASS BOOST %s\n", bass_boost_enabled ? "ON" : "OFF");
    }
}


#define BOLD() printf("%c[36m", 27);
#define NORMAL() printf("%c[0m", 27);

int main(int argc, char **argv)
{
	int r, tmp, fi, random=0;
	char Buffer[1024];
	char length_str[256], fade_str[256], volume[256], title_str[256];
	char tmp_str[256];
	char *tag;

	soundLowPass = 0;
	soundEcho = 0;
	soundQuality = 0;
    
	DetectSilence=1;
	silencelength=5;
	IgnoreTrackLength=0;
	DefaultLength=150000;
	TrailingSilence=1000;
	playforever=0;
	OutputFile = "";
	noinfo=0;

	while((r=getopt(argc, argv, "hlsrbieqW:L:t:"))>=0)
	{
		char *e;
		switch(r)
		{
			case 'h':
				printf("playgsf version %s (based on Highly Advanced version %s)\n\n",
						VERSION_STR, HA_VERSION_STR);
				printf("Usage: ./playgsf [options] files...\n\n");
				printf("  -l        Enable low pass filer\n");
				printf("  -s        Detect silence\n");
				printf("  -L        Set silence length in seconds (for detection). Default 5\n");
				printf("  -t        Set default track length in milliseconds. Default 150000 ms\n");
				printf("  -i        Ignore track length (use default length)\n");
				printf("  -e        Endless play\n");
				printf("  -r        Play files in random order\n");
				printf("  -W        output to the specified filename rather than soundcard\n");
				printf("  -q        Quiet; don't display informational output\n");
				printf("  -h        Displays what you are reading right now\n");
				return 0;
				break;
			case 'i':
				IgnoreTrackLength = 1;
				break;
			case 'l':
				soundLowPass = 1;
				break;
			case 's':
				DetectSilence = 1;
				break;
			case 'b':
                bass_boost_enabled = 1;
                break;
			case 'L':
				silencelength = strtol(optarg, &e, 0);
				if (e==optarg) {
					fprintf(stderr, "Bad value\n");
					return 1;
				}
				break;
			case 'e':
				playforever = 1;
				break;
			case 't':
				DefaultLength = strtol(optarg, &e, 0);
				if (e==optarg) {
					fprintf(stderr, "Bad value\n");
					return 1;
				}
				break;
			case 'r':
				random = 1;
				break;
			case 'W':
				fileoutput = 1;
				OutputFile = std::string(optarg);
				break;
			case 'q':
				noinfo = 1;
				break;
			case '?':
				fprintf(stderr, "Unknown argument. try -h\n");
				return 1;
				break;
		}
	}

	if (argc-optind<=0) {
		fprintf(stderr, "No files specified! For help, try -h\n");
		return 1;
	}


	if (random) { shuffle_list(&argv[optind], argc-optind); }

	if (!noinfo) {
		printf("playgsf version %s (based on Highly Advanced version %s)\n\n",
					VERSION_STR, HA_VERSION_STR);
	}

	signal(SIGINT, signal_handler);

	tag = (char*)malloc(50001);

	fi = optind;
    
	while (!g_must_exit && fi < argc)
	{
		decode_pos_ms = 0;
		seek_needed = -1;
		TrailingSilence=1000;

		r = GSFRun(argv[fi]);
		if (!r) {
			fi++;
			continue;
		}

		g_playing = 1;

		psftag_readfromfile((void*)tag, argv[fi]);

		if (!noinfo) {
			BOLD(); printf("Filename: "); NORMAL();
			printf("%s\n", basename(argv[fi]));
			BOLD(); printf("Channels: "); NORMAL();
			printf("%d\n", sndNumChannels);
			BOLD(); printf("Sample rate: "); NORMAL();
			printf("%d\n", sndSamplesPerSec);

			if (!psftag_getvar(tag, "title", title_str, sizeof(title_str)-1)) {
				BOLD(); printf("Title: "); NORMAL();
				printf("%s\n", title_str);
			}

			if (!psftag_getvar(tag, "artist", tmp_str, sizeof(tmp_str)-1)) {
				BOLD(); printf("Artist: "); NORMAL();
				printf("%s\n", tmp_str);
			}

			if (!psftag_getvar(tag, "game", tmp_str, sizeof(tmp_str)-1)) {
				BOLD(); printf("Game: "); NORMAL();
				printf("%s\n", tmp_str);
			}

			if (!psftag_getvar(tag, "year", tmp_str, sizeof(tmp_str)-1)) {
				BOLD(); printf("Year: "); NORMAL();
				printf("%s\n", tmp_str);
			}

			if (!psftag_getvar(tag, "copyright", tmp_str, sizeof(tmp_str)-1)) {
				BOLD(); printf("Copyright: "); NORMAL();
				printf("%s\n", tmp_str);
			}

			if (!psftag_getvar(tag, "gsfby", tmp_str, sizeof(tmp_str)-1)) {
				BOLD(); printf("GSF By: "); NORMAL();
				printf("%s\n", tmp_str);
			}

			if (!psftag_getvar(tag, "tagger", tmp_str, sizeof(tmp_str)-1)) {
				BOLD(); printf("Tagger: "); NORMAL();
				printf("%s\n", tmp_str);
			}

			if (!psftag_getvar(tag, "comment", tmp_str, sizeof(tmp_str)-1)) {
				BOLD(); printf("Comment: "); NORMAL();
				printf("%s\n", tmp_str);
			}

			if (!psftag_getvar(tag, "fade", fade_str, sizeof(fade_str)-1)) {
				FadeLength = LengthFromString(fade_str);
				BOLD(); printf("Fade: "); NORMAL();
				printf("%s (%d ms)\n", fade_str, FadeLength);
			} else {
			    strcpy(fade_str, "10");
			    FadeLength = LengthFromString(fade_str);
			    BOLD(); printf("Manual Fade: "); NORMAL();
			    printf("%s (%d ms)\n", fade_str, FadeLength);
			}

			if (!psftag_raw_getvar(tag, "length", length_str, sizeof(length_str)-1)) {
				TrackLength = LengthFromString(length_str) + FadeLength;
				BOLD(); printf("Length: "); NORMAL();
				printf("%s (%d ms) ", length_str, TrackLength);
				if (IgnoreTrackLength) {
					printf("(ignored)");
					TrackLength = DefaultLength;
				}
				printf("\n");
			}
			else {
				TrackLength = DefaultLength;
			}
		} else {
			if (!psftag_getvar(tag, "fade", fade_str, sizeof(fade_str)-1)) {
				FadeLength = LengthFromString(fade_str);
			} else {
			    strcpy(fade_str, "10");
			    FadeLength = LengthFromString(fade_str);
			    BOLD(); printf("Manual Fade: "); NORMAL();
			    printf("%s (%d ms)\n", fade_str, FadeLength);
			}
			
			if (!psftag_raw_getvar(tag, "length", length_str, sizeof(length_str)-1)) {
				TrackLength = LengthFromString(length_str) + FadeLength;
			} else {
				TrackLength = DefaultLength;
			}
		}

		/* Must be done after GSFrun so sndNumchannels and
		 * sndSamplesPerSec are set to valid values */
		int err;
		if ((err = snd_pcm_open(&pcm_handle, "default",
		                        SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
		    fprintf(stderr, "Error opening PCM device: %s\n", snd_strerror(err));
		    exit(1);
		}
		
		snd_pcm_hw_params_alloca(&hw_params);
		snd_pcm_hw_params_any(pcm_handle, hw_params);
		snd_pcm_hw_params_set_access(pcm_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
		snd_pcm_hw_params_set_format(pcm_handle, hw_params, SND_PCM_FORMAT_S16_LE);
		snd_pcm_hw_params_set_channels(pcm_handle, hw_params, sndNumChannels);
		snd_pcm_hw_params_set_rate(pcm_handle, hw_params, sndSamplesPerSec, 0);
		
		unsigned int rate = sndSamplesPerSec;
		snd_pcm_uframes_t buffer_size = 4096;
		snd_pcm_uframes_t period_size = 1024;
		
		lowshelf_init((float)sndSamplesPerSec, 250.0f, 5.0f);
		signal(SIGUSR2, handle_bass_toggle);
		
		snd_pcm_hw_params_set_buffer_size_near(pcm_handle, hw_params, &buffer_size);
		snd_pcm_hw_params_set_period_size_near(pcm_handle, hw_params, &period_size, NULL);
		
		if ((err = snd_pcm_hw_params(pcm_handle, hw_params)) < 0) {
		    fprintf(stderr, "Error setting HW parameters: %s\n", snd_strerror(err));
		    snd_pcm_close(pcm_handle);
		    exit(1);
		}
		
		snd_pcm_hw_params_get_period_size(hw_params, &frames, NULL);

		while(g_playing)
		{
			int remaining = TrackLength - (int)decode_pos_ms;
			if (remaining<0) {
				// this happens during silence period
				remaining = 0;
			}
			EmulationLoop();

			if (!noinfo) {
				BOLD(); printf("Time: "); NORMAL();
				printf("%02d:%02d.%02d ",
						(int)(decode_pos_ms/1000.0)/60,
						(int)(decode_pos_ms/1000.0)%60,
						(int)(decode_pos_ms/10.0)%100);
				if (!playforever) {
					/*BOLD();*/ printf("["); /*NORMAL();*/
					printf("%02d:%02d.%02d",
						remaining/1000/60, (remaining/1000)%60, (remaining/10%100)
							);
					/*BOLD();*/ printf("] of "); /*NORMAL();*/
					printf("%02d:%02d.%02d ",
						TrackLength/1000/60, (TrackLength/1000)%60, (TrackLength/10%100));
				}
				BOLD(); printf("  GBA Cpu: "); NORMAL();
				printf("%02d%% ", cpupercent);
				printf("     \r");

				fflush(stdout);
			}
		}
		if (!noinfo) {
			printf("\n--\n");
		}
        snd_pcm_drain(pcm_handle);
		fi++;
	}
	
    if (tag) {
        free(tag);
        tag = NULL;
    }
	
	if (pcm_handle) {
        snd_pcm_drop(pcm_handle);
        snd_pcm_close(pcm_handle);
        pcm_handle = NULL;
    }
	return 0;
}
