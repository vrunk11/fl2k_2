/*
 * osmo-fl2k, turns FL2000-based USB 3.0 to VGA adapters into
 * low cost DACs
 *
 * Copyright (C) 2016-2018 by Steve Markgraf <steve@steve-m.de>
 *
 * SPDX-License-Identifier: GPL-2.0+
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>
#include <getopt.h>
#include <unistd.h>

#ifndef _WIN32
	#include <unistd.h>
	#define sleep_ms(ms)	usleep(ms*1000)
	#else
	#include <windows.h>
	#include <io.h>
	#include <fcntl.h>
	#define sleep_ms(ms)	Sleep(ms)
#endif

#define _FILE_OFFSET_BITS 64

#ifdef _WIN64
#define FSEEK fseeko64
#else
#define FSEEK fseeko
#endif

#include "osmo-fl2k.h"

static fl2k_dev_t *dev = NULL;

soxr_t resampler_r = NULL;
soxr_t resampler_g = NULL;
soxr_t resampler_b = NULL;

static volatile int do_exit = 0;
static volatile int repeat = 1;

uint32_t input_sample_rate = 100000000;
uint32_t output_sample_rate = 100000000;

int resample = 0;

//buff size
uint32_t input_buf_size = FL2K_BUF_LEN;

//input file
FILE *file_r;
FILE *file_g;
FILE *file_b;

FILE *file2_r;
FILE *file2_g;
FILE *file2_b;
FILE *file_audio;

//input buffer
char *inbuf_r = NULL;
char *inbuf_g = NULL;
char *inbuf_b = NULL;

//resample buffer
short *resbuf_r = NULL;
short *resbuf_g = NULL;
short *resbuf_b = NULL;

//output buffer
char *outbuf_r = NULL;
char *outbuf_g = NULL;
char *outbuf_b = NULL;

//chanel activation
int red = 0;
int green = 0;
int blue = 0;
int red2 = 0;
int green2 = 0;
int blue2 = 0;
int audio = 0;

char sync_a = 'G';

//enable 16 bit to 8 bit conversion
int r16 = 0;
int g16 = 0;
int b16 = 0;

//signed = 1/ unsigned = 0
int r_sign = 0;
int g_sign = 0;
int b_sign = 0;

//if it's a tbc
int tbcR = 0;
int tbcG = 0;
int tbcB = 0;

//ire levels change
double ire_r = 0;
double ire_g = 0;
double ire_b = 0;

//chroma gain
double c_gain_r = 1;
double c_gain_g = 1;
double c_gain_b = 1;

//signal gain  (dynamic range)
double signal_gain_r = 1;
double signal_gain_g = 1;
double signal_gain_b = 1;

//Voltage control
double v_max_r = -1;
double v_max_g = -1;
double v_max_b = -1;

int max_value_r = 255;
int max_value_g = 255;
int max_value_b = 255;

int min_value_r = 0;
int min_value_g = 0;
int min_value_b = 0;

//combine mode
int cmb_mode_r = 0;
int cmb_mode_g = 0;
int cmb_mode_b = 0;

//read mode
int read_mode = 0;//0 = multitthreading / 1 = hybrid (R --> GB) / 2 = hybrid (RG --> B) / 3 = sequential (R -> G -> B)

//pipe mode
char pipe_mode = 'A';

int sample_type_r = 1;// 1 == signed   0 == unsigned
int sample_type_g = 1;// 1 == signed   0 == unsigned
int sample_type_b = 1;// 1 == signed   0 == unsigned

char video_standard = 0;

uint32_t sample_cnt_r = 0;//used for tbc processing
uint32_t sample_cnt_g = 0;//used for tbc processing
uint32_t sample_cnt_b = 0;//used for tbc processing

uint32_t line_cnt_r = 0;//used for tbc processing
uint32_t line_cnt_g = 0;//used for tbc processing
uint32_t line_cnt_b = 0;//used for tbc processing

uint32_t line_sample_cnt_r = 0;//used for tbc processing
uint32_t line_sample_cnt_g = 0;//used for tbc processing
uint32_t line_sample_cnt_b = 0;//used for tbc processing

uint32_t field_cnt_r = 0;//used for tbc processing
uint32_t field_cnt_g = 0;//used for tbc processing
uint32_t field_cnt_b = 0;//used for tbc processing

//unsigned char *pipe_buf = NULL;

//thread for processing
pthread_t thread_r;
pthread_t thread_g;
pthread_t thread_b;

//thread for resampling
pthread_t thread_r_res;
pthread_t thread_g_res;
pthread_t thread_b_res;

typedef struct soxr_resample_data {//used with soxr and pthread
	soxr_t soxr;
	fl2k_data_info_t *data_info;
	int *state_resample;
	int *state_process;
	char color;
} resample_data;

resample_data soxr_data_r;
resample_data soxr_data_g;
resample_data soxr_data_b;

//process synchronisation
//0 = initialisation | 1 = ready | 2 = processing | 3 = finished
int resample_r_state = 0;
int resample_g_state = 0;
int resample_b_state = 0;

int process_r_state = 0;
int process_g_state = 0;
int process_b_state = 0;

//pointer to variable
int *resample_r_state_ptr = &resample_r_state;
int *resample_g_state_ptr = &resample_g_state;
int *resample_b_state_ptr = &resample_b_state;

int *process_r_state_ptr = &process_r_state;
int *process_g_state_ptr = &process_g_state;
int *process_b_state_ptr = &process_b_state;

void usage(void)
{
	fprintf(stderr,
		"fl2k_file2, a sample player for FL2K VGA dongles\n\n"
		"Usage:\n"
		"\t[-d device_index (default: 0)]\n"
		"\t[-s samplerate (default: 100 MS/s) you can write(ntsc) or (pal)]\n"
		"\t[-u Set the output sample type of the fl2K to unsigned]\n"
		"\t[-R filename (use '-' to read from stdin)\n"
		"\t[-G filename (use '-' to read from stdin)\n"
		"\t[-B filename (use '-' to read from stdin)\n"
		"\t[-A audio file (use '-' to read from stdin)\n"
		"\t[-syncA chanel used for sync the audio file \ default : G \ value = (R ,G ,B)\n"
		"\t[-R2 secondary file to be combined with R (use '-' to read from stdin)\n"
		"\t[-G2 secondary file to be combined with G (use '-' to read from stdin)\n"
		"\t[-B2 secondary file to be combined with B (use '-' to read from stdin)\n"
		"\t[-R16 (convert bits 16 to 8)\n"
		"\t[-G16 (convert bits 16 to 8)\n"
		"\t[-B16 (convert bits 16 to 8)\n"
		"\t[-R8 interpret R input as 8 bit\n"
		"\t[-G8 interpret G input as 8 bit\n"
		"\t[-B8 interpret B input as 8 bit\n"
		"\t[-resample active output resampling\n"
		"\t[-signR interpret R input as (1 = signed / 0 = unsigned) or (s = signed / u = unsigned)\n"
		"\t[-signG interpret G input as (1 = signed / 0 = unsigned) or (s = signed / u = unsigned)\n"
		"\t[-signB interpret B input as (1 = signed / 0 = unsigned) or (s = signed / u = unsigned)\n"
		"\t[-cmbModeR combine mode \ default : 0 \ value = (0 ,1 ,2)\n"
		"\t[-cmbModeG combine mode \ default : 0 \ value = (0 ,1 ,2)\n"
		"\t[-cmbModeB combine mode \ default : 0 \ value = (0 ,1 ,2)\n"
		"\t[-tbcR interpret R as tbc file\n"
		"\t[-tbcG interpret G as tbc file\n"
		"\t[-tbcB interpret B as tbc file\n"
		"\t[-not_tbcR disable tbc processing for input R file\n"
		"\t[-not_tbcG disable tbc processing for input G file\n"
		"\t[-not_tbcB disable tbc processing for input B file\n"
		"\t[-CgainR chroma gain for input R (0.0 to 6.0) (using color burst)\n"
		"\t[-CgainG chroma gain for input G (0.0 to 6.0) (using color burst)\n"
		"\t[-CgainB chroma gain for input B (0.0 to 6.0) (using color burst)\n"
		"\t[-SgainR signal gain for output R (0.5 to 2.0) (clipping white)\n"
		"\t[-SgainG signal gain for output G (0.5 to 2.0) (clipping white)\n"
		"\t[-SgainB signal gain for output B (0.5 to 2.0) (clipping white)\n"
		"\t[-VmaxR maximum output voltage for channel R (0.003 to 0.7) (scale value) (disable Cgain and Sgain)\n"
		"\t[-VmaxG maximum output voltage for channel G (0.003 to 0.7) (scale value) (disable Cgain and Sgain)\n"
		"\t[-VmaxB maximum output voltage for channel B (0.003 to 0.7) (scale value) (disable Cgain and Sgain)\n"
		"\t[-MaxValueR max value for channel R (1 to 255) (reference level) (used for Vmax)\n"
		"\t[-MaxValueG max value for channel G (1 to 255) (reference level) (used for Vmax)\n"
		"\t[-MaxValueB max value for channel B (1 to 255) (reference level) (used for Vmax)\n"
		//"\t[-MinValueR min value for channel R (0 to 254) (reference level) (used for Vmax)\n"
		//"\t[-MinValueG min value for channel G (0 to 254) (reference level) (used for Vmax)\n"
		//"\t[-MinValueB min value for channel B (0 to 254) (reference level) (used for Vmax)\n"
		"\t[-ireR IRE level for input R (-50.0 to +50.0)\n"
		"\t[-ireG IRE level for input G (-50.0 to +50.0)\n"
		"\t[-ireB IRE level for input B (-50.0 to +50.0)\n"
		"\t[-FstartR seek to frame for input R\n"
		"\t[-FstartG seek to frame for input G\n"
		"\t[-FstartB seek to frame for input B\n"
		"\t[-audioOffset offset audio from a duration of x frame\n"
		"\t[-pipeMode (default = A) option : A = Audio file / R = output of R / G = output of G / B = output of B\n"
		"\t[-readMode (default = 0) option : 0 = multit-threading (RGB) / 1 = hybrid (R --> GB) / 2 = hybrid (RG --> B) / 3 = sequential (R -> G -> B)\n"
		"\n-info-version------------------------------------------------------\n\n"
		"runtime=%s API="SOXR_THIS_VERSION_STR"\n",
	soxr_version());
	exit(1);
}

const char *get_filename_ext(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if(!dot || dot == filename) return "";
    return dot + 1;
}

#ifdef _WIN32
BOOL WINAPI
sighandler(int signum)
{
	if (CTRL_C_EVENT == signum) {
		fprintf(stderr, "Signal caught, exiting!\n");
		fl2k_stop_tx(dev);
		do_exit = 1;
		return TRUE;
	}
	return FALSE;
}
#else
static void sighandler(int signum)
{
	fprintf(stderr, "Signal caught, exiting!\n");
	fl2k_stop_tx(dev);
	do_exit = 1;
}
#endif

static size_t soxr_input_fn(void * ibuf, soxr_cbuf_t * buf, size_t len)
{
	*buf = ibuf;
	//memcpy(ibuf,copyinbuf);
	return len+1;//+1 to avoid looping
}

void resampler_close(soxr_t soxr)
{
	soxr_delete(soxr);
}

void resampler_open(fl2k_data_info_t *data_info, soxr_t *soxr, uint32_t orate0, char color)
{
	double irate = 0;
	void * ibuf = NULL;
	size_t ilen = 0;
	
	if(color == 'R')
	{
		irate = (float)data_info->r_rate;
		ibuf = data_info->r_buf_res;
		ilen = data_info->r_buf_len;
	}
	if(color == 'G')
	{
		irate = (float)data_info->g_rate;
		ibuf = data_info->g_buf_res;
		ilen = data_info->g_buf_len;
	}
	if(color == 'B')
	{
		irate = (float)data_info->b_rate;
		ibuf = data_info->b_buf_res;
		ilen = data_info->b_buf_len;
	}
	
	unsigned int i = 0;
	char const *     const arg0 = "", * engine = "";
	
	double          const orate = (float)orate0;
	unsigned        const chans = (unsigned)1;//nb channel
	soxr_datatype_t const itype = (soxr_datatype_t)3;
	unsigned        const ospec = (soxr_datatype_t)11;
	unsigned long const q_recipe= 0;//SOXR_16_BITQ;//between (0-3) SOXR_16_BITQ = 3
	unsigned long const q_flags = 0;
	double   const passband_end = 0;
	double const stopband_begin = 0;
	double const phase_response = -1;
	int       const use_threads = 2;
	soxr_datatype_t const otype = ospec & 3;
	
	soxr_quality_spec_t       q_spec = soxr_quality_spec(q_recipe, q_flags);
	soxr_io_spec_t            io_spec = soxr_io_spec(itype, otype);
	soxr_runtime_spec_t const runtime_spec = soxr_runtime_spec(!use_threads);
	
	// Allocate resampling input and output buffers in proportion to the input
	// and output rates:
	size_t const osize = soxr_datatype_size(otype) * chans;
	size_t const isize = soxr_datatype_size(itype) * chans;
	size_t const olen = FL2K_BUF_LEN;
	size_t odone, clips = 0;
	soxr_error_t error;
	
	// Overrides (if given):
	if (passband_end   > 0) q_spec.passband_end   = passband_end / 100;
	if (stopband_begin > 0) q_spec.stopband_begin = stopband_begin / 100;
	if (phase_response >=0) q_spec.phase_response = phase_response;
	io_spec.flags = ospec & ~7u;
	
	// Create a stream resampler:
	*soxr = soxr_create(
	irate, orate, chans,         // Input rate, output rate, # of channels.
	&error,                         // To report any error during creation.
	&io_spec, &q_spec, &runtime_spec);
	
	if (!error)                      // Register input_fn with the resampler:
	{
		//set input buffer
		error = soxr_set_input_fn(*soxr, (soxr_input_fn_t)soxr_input_fn, ibuf, ilen);
	}

	fprintf(stderr,"engine inside = %s\n",soxr_engine(*soxr));
}

void fl2k_resample_to_freq(resample_data *soxr_data)
{
	soxr_t soxr = soxr_data->soxr;
	fl2k_data_info_t *data_info = soxr_data->data_info;
	char color = soxr_data->color;
	
	int resampled = 0;
	char *buf_out;
	short *buf_res;
	int *process_state = soxr_data->state_process;
	int *resample_state = soxr_data->state_resample;
	
	if(color == 'R')
	{
		resampled = data_info->r_sample_resampled;
		buf_out = data_info->r_buf;
		buf_res = data_info->r_buf_res;
	}
	if(color == 'G')
	{
		resampled = data_info->g_sample_resampled;
		buf_out = data_info->g_buf;
		buf_res = data_info->g_buf_res;
	}
	if(color == 'B')
	{
		resampled = data_info->b_sample_resampled;
		buf_out = data_info->b_buf;
		buf_res = data_info->b_buf_res;
	}
	
	unsigned int i = 0;
	size_t const olen = FL2K_BUF_LEN;
	void * const obuf = malloc(2 * olen);
	short *obuf16 = (void *)obuf;

	if(*resample_state != 0)//if not first call
	{
		//set status to ready
		*resample_state = 1;
			
		//wait fl2K_callback to be ready
		while(*process_state != 1){usleep(1);}
		
		//set status to processing
		//*resample_state = 2;
		//process data
		soxr_output(soxr, obuf, olen);
		//resize to 8bit and clip value
		//fprintf(stderr,"r\n");
		i = 0;
		while(i < FL2K_BUF_LEN)
		{
			if(obuf16[i] > 255)
			{
				buf_out[i] = 255;
			}
			else if(obuf16[i] < 0)
			{
				buf_out[i] = 0;
			}
			else
			{
				buf_out[i] = obuf16[i];
			}
			
			i++;
		}
	}
	else//initialisation
	{
		//set status to ready
		*resample_state = 1;
			
		//wait fl2K_callback to be ready
		while(*process_state != 1){usleep(1);}
		//set status to processing
		//*resample_state = 2;
		//set empty buffer while first data are processed
		i = 0;
		while(i < FL2K_BUF_LEN)
		{
			buf_out[i] = 0;
			i++;
		}
	}
	
	//set status to finished
	*resample_state = 3;
	//wait fl2K_callback to finish
	while(*process_state != 3){usleep(1);}
	
	free(obuf);
}

//compute number of sample to skip
unsigned long calc_nb_skip(long sample_cnt,int linelength,long frame_lengt,long bufsize,char standard)
{
	int nb_skip = 0;
	
	//on enlève ce qui reste avant le skip
	bufsize -= (frame_lengt - sample_cnt);
	nb_skip = 1;
	
	while(bufsize > 0)
	{
		bufsize -= frame_lengt;
		
		//if we can do a complet skip
		if((bufsize - linelength) > 0)
		{
			nb_skip ++;
		}
		//if we stop in the middle of a skip
		else if(((bufsize - linelength) < 0) && bufsize > 0)
		{
			nb_skip ++;
		}
	}
	
	if(standard == 'P')
	{
		linelength -= 8;//remove the 4 extra sample from the last line
	}
	
	return (nb_skip * linelength);//multiply for giving the number of byte to skip
}

int read_sample_file(void *inpt_color)
{
	//parametter
	char *buffer = NULL;
	short *resbuffer = malloc(input_buf_size*2);//used for cast 8 bit to 16 bit
	FILE *stream = NULL;
	FILE *stream2 = NULL;
	FILE *streamA = NULL;
	int istbc = 0;
	char color = (char *) inpt_color;
	//uint32_t sample_rate = input_sample_rate;
	double *chroma_gain = NULL;
	double *ire_level = NULL;
	double signal_gain = 1;
	double v_max = -1;
	int max_value = 255;
	
	int is16 = 0;
	int is_signed = 0;
	int is_stereo = 0;
	int combine_mode = 0;
	int is_sync_a = 0;
	int use_pipe = 0;
	
	long i = 0;//counter for tmp_buf
	long y = 0;//counter for calc
	
	//(NTSC line = 910 frame = 477750) (PAL line = 1135 frame = 709375)
	unsigned long frame_lengt = 0;
	unsigned long frame_nb_line = 0;
	unsigned int v_start =0;
	unsigned int v_end =0;
	unsigned long line_lengt = 0;
	unsigned long sample_skip = 0;
	unsigned int audio_frame = 0;
	
	//COLOR BURST
	unsigned int cbust_sample = 0;
	unsigned int cbust_middle = 0;
	double cbust_count = 0;
	double cbust_offset = 0;
	unsigned int cbust_start = 0;
	unsigned int cbust_end = 0;
	
	//count
	uint32_t *sample_cnt = NULL;
	uint32_t *line_cnt = NULL;
	uint32_t *line_sample_cnt = NULL;
	uint32_t *field_cnt = NULL;
	
	int *process_state = NULL;
	int *resample_state = NULL;

	if(color == 'R')
	{
		process_state = process_r_state_ptr;
		resample_state = resample_r_state_ptr;
		buffer = inbuf_r;
		stream = file_r;
		stream2 = file2_r;
		istbc = tbcR;
		sample_cnt = &sample_cnt_r;
		line_cnt = &line_cnt_r;
		line_sample_cnt = &line_sample_cnt_r;
		field_cnt = & field_cnt_r;
		chroma_gain = &c_gain_r;
		ire_level = &ire_r;
		is_stereo = red2;
		combine_mode = cmb_mode_r;
		is16 = r16;
		is_signed = r_sign;
		signal_gain = signal_gain_r;
		v_max = v_max_r;
		max_value = max_value_r;
		if(sync_a == 'R' && pipe_mode == 'A')
		{
			is_sync_a = 1;
			streamA = file_audio;
		}
		else if(pipe_mode == 'R')
		{
			use_pipe = 1;
		}
	}
	else if(color == 'G')
	{
		process_state = process_g_state_ptr;
		resample_state = resample_g_state_ptr;
		buffer = inbuf_g;
		stream = file_g;
		stream2 = file2_g;
		istbc = tbcG;
		sample_cnt = &sample_cnt_g;
		line_cnt = &line_cnt_g;
		line_sample_cnt = &line_sample_cnt_g;
		field_cnt = & field_cnt_g;
		chroma_gain = &c_gain_g;
		ire_level = &ire_g;
		is_stereo = green2;
		combine_mode = cmb_mode_g;
		is16 = g16;
		is_signed = g_sign;
		signal_gain = signal_gain_g;
		v_max = v_max_g;
		max_value = max_value_g;
		if(sync_a == 'G' && pipe_mode == 'A')
		{
			is_sync_a = 1;
			streamA = file_audio;
		}
		else if(pipe_mode == 'G')
		{
			use_pipe = 1;
		}
	}
	else if(color == 'B')
	{
		process_state = process_b_state_ptr;
		resample_state = resample_b_state_ptr;
		buffer = inbuf_b;
		stream = file_b;
		stream2 = file2_b;
		istbc = tbcB;
		sample_cnt = &sample_cnt_b;
		line_cnt = &line_cnt_b;
		line_sample_cnt = &line_sample_cnt_b;
		field_cnt = & field_cnt_b;
		chroma_gain = &c_gain_b;
		ire_level = &ire_b;
		is_stereo = blue2;
		combine_mode = cmb_mode_b;
		is16 = b16;
		is_signed = b_sign;
		signal_gain = signal_gain_b;
		v_max = v_max_b;
		max_value = max_value_b;
		if(sync_a == 'B' && pipe_mode == 'A')
		{
			is_sync_a = 1;
			streamA = file_audio;
		}
		else if(pipe_mode == 'B')
		{
			use_pipe = 1;
		}
	}
	
	//IRE
	const float ire_conv = 1.59375;// (255/160)
	const float ire_min = 63.75;//40 * (255/160)
	const float ire_new_max = 159.375;// (140 * (255/160)) - (40 * (255/160))
	const float ire_add = (*ire_level * ire_conv);
	const float ire_gain = (ire_new_max / (ire_new_max + ire_add));
	double ire_tmp = 0;
	
	if(video_standard == 'P')//PAL value multiplied by 2 if input is 16bit
	{
		frame_lengt = 709379 * (1 + is16);//709375 + 4 extra sample
		line_lengt = 1135 * (1 + is16);
		frame_nb_line = 625;
		v_start = 185 * (1 + is16);
		v_end = 1107 * (1 + is16);
		cbust_start = 98 * (1 + is16);//not set
		cbust_end = 138 * (1 + is16);//not set
		audio_frame = ((88200/25) *2);
		//sample_skip = 4 * (1 + is16);//remove 4 extra sample in pal
	}
	else if(video_standard == 'N')//NTSC value multiplied by 2 if input is 16bit
	{
		frame_lengt = 477750 * (1 + is16);
		line_lengt = 910 * (1 + is16);
		frame_nb_line = 525;
		v_start = 134 * (1 + is16);
		v_end = 894 * (1 + is16);
		cbust_start = 78 * (1 + is16);
		cbust_end = 110 * (1 + is16);
		audio_frame = ((88200/30) * 2);
	}
	
	unsigned long buf_size = (input_buf_size + (is16 * input_buf_size));
	
	if(istbc == 1)//compute buf size
	{
		sample_skip += calc_nb_skip(*sample_cnt,line_lengt,frame_lengt,buf_size,video_standard);
	}
	
	buf_size += sample_skip;
	
	unsigned char *tmp_buf = malloc(input_buf_size);//8bit data so we can use input_buf_size
	unsigned char *audio_buf = malloc(audio_frame);
	char *audio_buf_signed = (void *)audio_buf;
	unsigned char *calc = malloc(buf_size);
	unsigned char *calc2 = malloc(buf_size);
	unsigned short value16 = 0;
	unsigned short value16_2 = 0;
	unsigned char value8 = 0;
	unsigned char value8_2 = 0;
	
	short *value16_signed = (void *)&value16;
	short *value16_2_signed = (void *)&value16_2;
	char *value8_signed = (void *)&value8;
	char *value8_2_signed = (void *)&value8_2;
	
	//set status to ready
	*process_state = 1;
	//wait resampler
	if(resample)while(*resample_state != 1){usleep(1);}
	//set status to processing
	//*process_state = 2;
	
	if (tmp_buf == NULL || calc == NULL)
	{
		free(tmp_buf);   // Free both in case only one was allocated
		free(calc);
		fprintf(stderr, "(%c) malloc error (tmp_buf , calc)\n",color);
		return -1;
	}
	
	if(istbc == 1)
	{
		sample_skip = calc_nb_skip(*sample_cnt,line_lengt,frame_lengt,buf_size,video_standard);
	}
	
	if(is_stereo)
	{
		if(fread(calc,buf_size,1,stream) != 1 || fread(calc2,buf_size,1,stream2) != 1)
		{
			free(tmp_buf);   // Free both in case only one was allocated
			free(calc);
			free(calc2);
			fprintf(stderr, "(%c) fread error %d : ",color,errno);
			perror(NULL);
			return -1;
		}
	}
	else
	{
		if(fread(calc,buf_size,1,stream) != 1)
		{
			free(tmp_buf);   // Free both in case only one was allocated
			free(calc);
			free(calc2);
			fprintf(stderr, "(%c) fread error %d : ",color,errno);
			perror(NULL);
			return -1;
		}
	}

	while((y < buf_size) && !do_exit)
	{	
		//if we are at then end of the frame skip one line
		if(*sample_cnt >= frame_lengt)
		{
			if(istbc == 1)
			{
				if(video_standard == 'P')
				{
					//skip 1 line - 4 sample
					y += (line_lengt - 8);
				}
				else
				{
					//skip 1 line
					y += line_lengt;
				}
				*sample_cnt = 0;
			}
			
			//write audio file to stdout only if its not a terminal
			if(isatty(STDOUT_FILENO) == 0 && is_sync_a)
			{
				//write(stdout, tmp_buf, input_buf_size);
				fread(audio_buf_signed,audio_frame,1,streamA);
				//write(stdout, audio_buf_signed, audio_frame);
				fwrite(audio_buf, audio_frame,1,stdout);
				fflush(stdout);
			}
		}
		
		if(is16 == 1)
		{
			value16 = ((calc[y+1] * 256) + calc[y]);
			if(is_stereo)
			{
				value16_2 = ((calc2[y+1] * 256) + calc2[y]);
			}
			y += 2;
		}
		else
		{
			value8 = calc[y];
			if(is_stereo)
			{
				value8_2 = calc2[y];
			}
			y += 1;
		}
		
		//convert 16bit to 8bit and combine
		if(is16 == 1)
		{
			if(is_stereo)
			{
				if(combine_mode == 0)//default
				{
					if((round((*value16_signed + *value16_2_signed)/ 256.0) + 128) < -128)
					{
						tmp_buf[i] = -128;
					}
					/*else if((round((*value16_signed + *value16_2_signed)/ 256.0) + 128) > 0)
					{
						tmp_buf[i] = 0;
					}*/
					else
					{
						tmp_buf[i] = round((*value16_signed + *value16_2_signed)/ 256.0) + 128;//convert to 8 bit
					}
				}
				else if(combine_mode == 2)
				{
					if(((*line_sample_cnt >= v_start) && (*line_sample_cnt <= v_end))&& *line_cnt > (22 + ((unsigned long)*field_cnt % 2)) )
					{
						tmp_buf[i] = round(round((value16)  / 256.0) / 1.34) + 64;//convert to 8 bit
					}
					else
					{
						tmp_buf[i] = round(value16_2 / 256.0);//convert to 8 bit
					}
					
				}
				else//mode 1
				{
					tmp_buf[i] = round(((value16 + value16_2)/2)/ 256.0);//convert to 8 bit
				}
				
			}
			else
			{
				tmp_buf[i] = round(value16 / 256.0);//convert to 8 bit
			}
		}
		else if(is_stereo)//combine 2 file
		{
			if(combine_mode == 0)//default
			{
				tmp_buf[i] = *value8_signed + *value8_2_signed + 128;
			}
			else//mode 1
			{
				tmp_buf[i] = round((value8 + value8_2)/2);
			}
		}
		else//no processing
		{
			tmp_buf[i] = value8;
		}
		
		if(*chroma_gain != 1)
		{
			//color burst reading
			if(((*line_sample_cnt >= cbust_start) && (*line_sample_cnt <= cbust_end)) && (*line_cnt == (21 + ((unsigned long)*field_cnt % 2))))
			{
				cbust_sample = tmp_buf[i];
				cbust_count += 1;
				cbust_middle = cbust_sample / cbust_count;
				cbust_offset = (cbust_middle - (cbust_middle / *chroma_gain));
			}
			
			//chroma gain
			
			if(((*line_sample_cnt >= cbust_start) && (*line_sample_cnt <= cbust_end * 1 + is16))&& (*line_cnt > (22 + ((unsigned long)*field_cnt % 2))))
			{
				tmp_buf[i] = round(tmp_buf[i] / *chroma_gain);// + cbust_offset;
			}
		}
		
		//ire 7.5 to ire 0
		if (*ire_level != 0)
		{
			if(((*line_sample_cnt >= v_start) && (*line_sample_cnt <= v_end))&& *line_cnt > (22 + ((unsigned long)*field_cnt % 2)) )
			{
				ire_tmp = (tmp_buf[i] - ire_min);
				
				if(ire_tmp < 0)//clipping value
				{
					ire_tmp = 0;
				}
				ire_tmp = ire_tmp * ire_gain;
				tmp_buf[i] =  round(ire_tmp + ire_add + ire_min);
			}
		}
		
		//signal gain
		if(signal_gain != 1)
		{
			if(tmp_buf[i] > 5)
			{
				if((tmp_buf[i] * signal_gain) > 255)
				{
					tmp_buf[i] = 255;
				}
				else
				{
					tmp_buf[i] = round(tmp_buf[i] * signal_gain);
				}
			}
		}
		
		//scale to max voltage
		if(v_max > 0.0)
		{
			if(round(tmp_buf[i]*(255/max_value)) > 255)
			{
				tmp_buf[i] = 255;
			}
			else
			{
				tmp_buf[i] = round((tmp_buf[i]*(255/max_value))/(0.7/v_max));
			}
		}
		
		//fix sign and cast to 16 bit
		if(!is_signed)
		{
			tmp_buf[i] = tmp_buf[i] - 128;
			resbuffer[i] = tmp_buf[i];
		}
		else
		{
			resbuffer[i] = tmp_buf[i];
		}
		
		i += 1;//on avance tmp_buff de 1
		
		if(*line_cnt == ((frame_nb_line / 2) + ((unsigned long)*field_cnt % 2)))//field 1 = (max - 0.5)   field 2 = (max + 0.5)
		{
			*line_cnt = 0;
			*field_cnt += 1;
			cbust_sample = 0;
			cbust_middle = 0;
			cbust_count = 0;
		}
		
		if(*line_sample_cnt == line_lengt)
		{
			*line_sample_cnt = 0;
			*line_cnt += 1;
		}
		
		*sample_cnt += (1 + is16);
		*line_sample_cnt += (1 + is16);
	}
	
	//set status to finished
	*process_state = 3;
	//wait resampler to finish
	if(resample)while(*resample_state != 3){usleep(1);}
	
	if(resample)
	{
		if(color == 'R')
		{
			memcpy(resbuf_r, resbuffer, input_buf_size*2);
		}
		if(color == 'G')
		{
			memcpy(resbuf_g, resbuffer, input_buf_size*2);
		}
		if(color == 'B')
		{
			memcpy(resbuf_b, resbuffer, input_buf_size*2);
		}
	}
	else
	{
		if(color == 'R')
		{
			memcpy(outbuf_r, tmp_buf, input_buf_size);
		}
		if(color == 'G')
		{
			memcpy(outbuf_g, tmp_buf, input_buf_size);
		}
		if(color == 'B')
		{
			memcpy(outbuf_b, tmp_buf, input_buf_size);
		}
	}
	
	if(isatty(STDOUT_FILENO) == 0 && use_pipe)
	{
		fwrite(tmp_buf, input_buf_size,1,stdout);
		fflush(stdout);
	}
	
	free(resbuffer);
	free(tmp_buf);
	free(audio_buf);
	free(calc);
	free(calc2);
	return 0;
}

void fl2k_callback(fl2k_data_info_t *data_info)
{	
	static uint32_t repeat_cnt = 0;
	
	//store the number of block readed
	int r;
	int g;
	int b;

	if (data_info->device_error) {
		fprintf(stderr, "Device error, exiting.\n");
		do_exit = 1;
		return;
	}
	
	//set sign (signed = 1 , unsigned = 0)
	data_info->sampletype_signed_r = sample_type_r;
	data_info->sampletype_signed_g = sample_type_g;
	data_info->sampletype_signed_b = sample_type_b;
	
	//send the bufer with a size of (1280 * 1024) = 1310720
	if(red == 1)
	{
		data_info->r_buf_res = resbuf_r;
		data_info->r_buf = outbuf_r;
		data_info->r_buf_len = input_buf_size*2;
		data_info->r_rate = input_sample_rate;
		data_info->r_sample_resampled = resample;
	}
	if(green == 1)
	{
		data_info->g_buf_res = resbuf_g;
		data_info->g_buf = outbuf_g;
		data_info->g_buf_len = input_buf_size*2;
		data_info->g_rate = input_sample_rate;
		data_info->g_sample_resampled = resample;
	}
	if(blue == 1)
	{
		data_info->b_buf_res = resbuf_b;
		data_info->b_buf = outbuf_b;
		data_info->b_buf_len = input_buf_size*2;
		data_info->b_rate = input_sample_rate;
		data_info->b_sample_resampled = resample;
	}
	
	//initialisation
	//start resampler if not initialisaed
	if(soxr_data_r.state_process == 0 && red == 1)
	{
		resampler_open(data_info, &resampler_r,fl2k_get_sample_rate(dev), 'R');
		//resampling data
		soxr_data_r.soxr = resampler_r;
		soxr_data_r.state_process = &process_r_state;
		soxr_data_r.state_resample = &resample_r_state;
		soxr_data_r.data_info = data_info;
		soxr_data_r.color = 'R';
	}
	if(soxr_data_g.state_process == 0 && green == 1)
	{
		resampler_open(data_info, &resampler_g, fl2k_get_sample_rate(dev), 'G');
		fprintf(stderr,"engine outside = %s\n",soxr_engine(resampler_g));
		//resampling data
		soxr_data_g.soxr = resampler_g;
		soxr_data_g.state_process = &process_g_state;
		soxr_data_g.state_resample = &resample_g_state;
		soxr_data_g.data_info = data_info;
		soxr_data_g.color = 'G';
	}
	if(soxr_data_b.state_process == 0 && blue == 1)
	{
		resampler_open(data_info, &resampler_b, fl2k_get_sample_rate(dev), 'B');
		//resampling data
		soxr_data_b.soxr = resampler_b;
		soxr_data_b.state_process = &process_b_state;
		soxr_data_b.state_resample = &resample_b_state;
		soxr_data_b.data_info = data_info;
		soxr_data_b.color = 'B';
	}
	
	//read until buffer is full
	//RED
	if(red == 1 && !feof(file_r))
	{
		//process file
		pthread_create(&thread_r,NULL,read_sample_file,'R');
		//resample
		if(resample)pthread_create(&thread_r_res,NULL,fl2k_resample_to_freq,&soxr_data_r);

		if (ferror(file_r))
		{
			fprintf(stderr, "(RED) : File Error\n");
		}
	}
	else if(red == 1 && feof(file_r))
	{
		fprintf(stderr, "(RED) : Nothing more to read\n");
	}
	
	//thread sync R
	if(read_mode == 3 || read_mode == 1)
	{
		if(red == 1)
		{
			pthread_join(thread_r,NULL);
			if(resample)pthread_join(thread_r_res,NULL);
		}
	}
	
	//GREEN
	if(green == 1 && !feof(file_g))
	{
		pthread_create(&thread_g,NULL,read_sample_file,'G');
		//resample
		if(resample)pthread_create(&thread_g_res,NULL,fl2k_resample_to_freq,&soxr_data_g);
		
		if (ferror(file_g))
		{
			fprintf(stderr, "(GREEN) : File Error\n");
		}
	}
	else if(green == 1 && feof(file_g))
	{
		fprintf(stderr, "(GREEN) : Nothing more to read\n");
	}
	
	//thread sync G
	if(read_mode == 3)
	{
		if(green == 1)
		{
			pthread_join(thread_g,NULL);
			if(resample)pthread_join(thread_g_res,NULL);
		}
	}
	else if(read_mode == 2)
	{
		if(red == 1)
		{
			pthread_join(thread_r,NULL);
			if(resample)pthread_join(thread_r_res,NULL);
		}
		if(green == 1)
		{
			pthread_join(thread_g,NULL);
			if(resample)pthread_join(thread_g_res,NULL);
		}
	}
	
	//BLUE
	if(blue == 1 && !feof(file_b))
	{
		pthread_create(&thread_b,NULL,read_sample_file,'B');
		//resample
		if(resample)pthread_create(&thread_b_res,NULL,fl2k_resample_to_freq,&soxr_data_b);
		
		if(ferror(file_b))
		{
			fprintf(stderr, "(BLUE) : File Error\n");
		}
	}
	else if(blue == 1 && feof(file_b))
	{
		fprintf(stderr, "(BLUE) : Nothing more to read\n");
	}
	
	//thread sync B
	if(read_mode == 0)
	{
		if(red == 1)
		{
			pthread_join(thread_r,NULL);
			if(resample)pthread_join(thread_r_res,NULL);
		}
		if(green == 1)
		{
			pthread_join(thread_g,NULL);
			if(resample)pthread_join(thread_g_res,NULL);
		}
		if(blue == 1)
		{
			pthread_join(thread_b,NULL);
			if(resample)pthread_join(thread_b_res,NULL);
		}
	}
	else if(read_mode == 3 || read_mode == 2)
	{
		if(blue == 1)
		{
			pthread_join(thread_b,NULL);
			if(resample)pthread_join(thread_b_res,NULL);
		}
	}
	else if(read_mode == 1)
	{
		if(green == 1)
		{
			pthread_join(thread_g,NULL);
			if(resample)pthread_join(thread_g_res,NULL);
		}
		if(blue == 1)
		{
			pthread_join(thread_b,NULL);
			if(resample)pthread_join(thread_b_res,NULL);
		}
	}
	
	//close threads
	/*if(red == 1)
	{
		pthread_exit(thread_r);
		pthread_exit(thread_r_res);
	}
	if(green == 1)
	{
		pthread_exit(thread_g);
		pthread_exit(thread_g_res);
	}
	if(blue == 1)
	{
		pthread_exit(thread_b);
		pthread_exit(thread_b_res);
	}*/
	
	if((red == 0 || feof(file_r)) && (green == 0 || feof(file_g)) && (blue == 0 || feof(file_b)))
	{
		fprintf(stderr, "End of the process\n");
		fl2k_stop_tx(dev);
		do_exit = 1;
	}
	
	/*if(!(green == 1 && feof(file_g)) || !(green == 1 && feof(file_g)) && !(green == 1 && feof(file_g)))
	{
		
	}*/
	/*if(((r <= 0) && (red == 1)) || ((g <= 0)  && (green == 1))|| ((b <= 0) && (blue == 1)))
	{
		fl2k_stop_tx(dev);
		do_exit = 1;
	}*/
}

int main(int argc, char **argv)
{
#ifndef _WIN32
	struct sigaction sigact, sigign;
#endif

#ifdef _WIN32 || _WIN64
	_setmode(_fileno(stdout), O_BINARY);
	_setmode(_fileno(stdin), O_BINARY);	
#endif

	int r, opt, i;
	uint32_t buf_num = 0;
	int dev_index = 0;
	void *status;
	
	int override_r16 = -1;
	int override_g16 = -1;
	int override_b16 = -1;
	int override_r_sign = -1;
	int override_g_sign = -1;
	int override_b_sign = -1;
	int override_tbc_r = -1;
	int override_tbc_g = -1;
	int override_tbc_b = -1;
	
	uint64_t start_r = 0;
	uint64_t start_g = 0;
	uint64_t start_b = 0;
	uint64_t start_audio = 0;
	
	long audio_offset = 0;

	//file adress
	char *filename_r = NULL;
	char *filename_g = NULL;
	char *filename_b = NULL;
	
	char *filename2_r = NULL;
	char *filename2_g = NULL;
	char *filename2_b = NULL;
	char *filename_audio = NULL;
	
	//pipe_buf = malloc(input_buf_size);
	
	/*if (pipe_buf == NULL)
	{
		free(pipe_buf);   // Free both in case only one was allocated
		fprintf(stderr, "malloc error (pipe_buf)\n");
		return -1;
	}*/
	
	//setvbuf(stdout,pipe_buf,_IOLBF,input_buf_size);
	
	int option_index = 0;
	static struct option long_options[] = {
		{"R16", 0, 0, 1},
		{"G16", 0, 0, 2},
		{"B16", 0, 0, 3},
		{"tbcR", 0, 0, 4},
		{"tbcG", 0, 0, 5},
		{"tbcB", 0, 0, 6},
		{"readMode", 1, 0, 7},
		{"CgainR", 1, 0, 8},
		{"CgainG", 1, 0, 9},
		{"CgainB", 1, 0, 10},
		{"ireR", 1, 0, 11},
		{"ireG", 1, 0, 12},
		{"ireB", 1, 0, 13},
		{"FstartR", 1, 0, 14},
		{"FstartG", 1, 0, 15},
		{"FstartB", 1, 0, 16},
		{"FstartA", 1, 0, 16},
		{"R2", 1, 0, 17},
		{"G2", 1, 0, 18},
		{"B2", 1, 0, 19},
		{"syncA", 1, 0, 20},
		{"cmbModeR", 1, 0, 21},
		{"cmbModeG", 1, 0, 22},
		{"cmbModeB", 1, 0, 23},
		{"audioOffset", 1, 0, 24},
		{"pipeMode", 1, 0, 25},
		{"SgainR", 1, 0, 26},
		{"SgainG", 1, 0, 27},
		{"SgainB", 1, 0, 28},
		{"R8", 0, 0, 29},
		{"G8", 0, 0, 30},
		{"B8", 0, 0, 31},
		{"not_tbcR", 0, 0, 32},
		{"not_tbcG", 0, 0, 33},
		{"not_tbcB", 0, 0, 34},
		{"signR", 1, 0, 35},
		{"signG", 1, 0, 36},
		{"signB", 1, 0, 37},
		{"VmaxR", 1, 0, 38},
		{"VmaxG", 1, 0, 39},
		{"VmaxB", 1, 0, 40},
		{"MaxValueR", 1, 0, 41},
		{"MaxValueG", 1, 0, 42},
		{"MaxValueB", 1, 0, 43},
		{"resample", 0, 0, 44},
		{0, 0, 0, 0}//reminder : letter value are from 65 to 122
	};

	while ((opt = getopt_long_only(argc, argv, "d:r:s:uR:G:B:A:",long_options, &option_index)) != -1) {
		switch (opt) {
		case 'd':
			dev_index = (uint32_t)atoi(optarg);
			break;
		case 'r':
			repeat = (int)atoi(optarg);
			break;
		case 's':
			if((strcmp(optarg, "ntsc" ) == 0) || (strcmp(optarg, "NTSC" ) == 0) || (strcmp(optarg, "Ntsc" ) == 0))
			{
				input_sample_rate = (uint32_t) 14318181;
			}
			else if((strcmp(optarg, "pal" ) == 0) || (strcmp(optarg, "PAL" ) == 0) || (strcmp(optarg, "Pal" ) == 0))
			{
				input_sample_rate = (uint32_t) 17734475;
			}
			else
			{
				input_sample_rate = (uint32_t)atof(optarg);
			}
			break;
		case 'u':
			sample_type_r = 0;
			sample_type_g = 0;
			sample_type_b = 0;
			break;
		case 'R':
			red = 1;
			filename_r = optarg;
			break;
		case 'G':
			green = 1;
			filename_g = optarg;
			break;
		case 'B':
			blue = 1;
			filename_b = optarg;
			break;
		case 'A':
			audio = 1;
			filename_audio = optarg;
			break;
		case 1:
			override_r16 = 1;
			break;
		case 2:
			override_g16 = 1;
			break;
		case 3:
			override_b16 = 1;
			break;
		case 4:
			override_tbc_r = 1;
			break;
		case 5:
			override_tbc_g = 1;
			break;
		case 6:
			override_tbc_b = 1;
			break;
		case 7:
			read_mode = (int)atoi(optarg);
			break;
		case 8:
			c_gain_r = atof(optarg);
			break;
		case 9:
			c_gain_g = atof(optarg);
			break;
		case 10:
			c_gain_b = atof(optarg);
			break;
		case 11:
			ire_r = atof(optarg);
			break;
		case 12:
			ire_g = atof(optarg);
			break;
		case 13:
			ire_b = atof(optarg);
			break;
		case 14:
			start_r = atoi(optarg);
			break;
		case 15:
			start_g = atoi(optarg);
			break;
		case 16:
			start_b = atoi(optarg);
			break;
		case 17:
			red2 = 1;
			filename2_r = optarg;
			break;
		case 18:
			green2 = 1;
			filename2_g = optarg;
			break;
		case 19:
			blue2 = 1;
			filename2_b = optarg;
			break;
		case 20:
			if(*optarg == 'r'){sync_a = 'R';}
			else if(*optarg == 'g'){sync_a = 'G';}
			else if(*optarg == 'b'){sync_a = 'B';}
			else{sync_a = *optarg;}
			break;
		case 21:
			cmb_mode_r = atoi(optarg);
			break;
		case 22:
			cmb_mode_g = atoi(optarg);
			break;
		case 23:
			cmb_mode_b = atoi(optarg);
			break;
		case 24:
			audio_offset = atol(optarg);
			break;
		case 25:
			if(*optarg == 'a'){pipe_mode = 'A';}
			else if(*optarg == 'r'){pipe_mode = 'R';}
			else if(*optarg == 'g'){pipe_mode = 'G';}
			else if(*optarg == 'b'){pipe_mode = 'B';}
			else{pipe_mode = *optarg;}
			break;
		case 26:
			signal_gain_r = atof(optarg);
			break;
		case 27:
			signal_gain_g = atof(optarg);
			break;
		case 28:
			signal_gain_b = atof(optarg);
			break;
		case 29:
			override_r16 = 0;
			break;
		case 30:
			override_g16 = 0;
			break;
		case 31:
			override_b16 = 0;
			break;
		case 32:
			override_tbc_r = 0;
			break;
		case 33:
			override_tbc_g = 0;
			break;
		case 34:
			override_tbc_b = 0;
			break;
		case 35:
			if(*optarg == 'u' || *optarg == 'U'){override_r_sign = 0;}
			else if(*optarg == 's' || *optarg == 'S'){override_r_sign = 1;}
			else{override_r_sign = atoi(optarg);}
			break;
		case 36:
			if(*optarg == 'u' || *optarg == 'U'){override_g_sign = 0;}
			else if(*optarg == 's' || *optarg == 'S'){override_g_sign = 1;}
			else{override_g_sign = atoi(optarg);}
			break;
		case 37:
			if(*optarg == 'u' || *optarg == 'U'){override_b_sign = 0;}
			else if(*optarg == 's' || *optarg == 'S'){override_b_sign = 1;}
			else{override_b_sign = atoi(optarg);}
			break;
		case 38:
			v_max_r = atof(optarg);
			break;
		case 39:
			v_max_g = atof(optarg);
			break;
		case 40:
			v_max_b = atof(optarg);
			break;
		case 41:
			max_value_r = atoi(optarg);
			break;
		case 42:
			max_value_g = atoi(optarg);
			break;
		case 43:
			max_value_b = atoi(optarg);
			break;
		case 44:
			resample = 1;
			break;
		default:
			usage();
			break;
		}
	}

	if (dev_index < 0)
	{
		fprintf(stderr, "\nDevice number invalid\n\n");
		usage();
	}
	
	if(read_mode < 0 || read_mode > 3)
	{
		fprintf(stderr, "\nRead mode unknown\n\n");
		usage();
	}

	if(red == 0 && green == 0 && blue == 0)
	{
		fprintf(stderr, "\nNo file provided using option (-R,-G,-B)\n\n");
		usage();
	}
	
	if((red == 0 && red2 == 1) || (green == 0 && green2 == 1) || (blue == 0 && blue2 == 1))
	{
		fprintf(stderr, "\nNo main file provided using (-R,-G,-B)\n\n");
		usage();
	}
	
	if((override_r_sign < -1 || override_r_sign > 1) || (override_g_sign < -1 || override_g_sign > 1) || (override_b_sign < -1 || override_b_sign > 1))
	{
		fprintf(stderr, "\nInvalid value for one of the option (-signR,-signG,-signB) value : (0,u,1,s)\n\n");
		usage();
	}
	
	if((sync_a != 'R') && (sync_a != 'G') && (sync_a != 'B'))
	{
		fprintf(stderr, "\nUnknow parametter '%c' for option -syncA / value : (R,r,G,g,B,b)\n\n",sync_a);
		usage();
	}
	else if(sync_a == 'R'){start_audio = start_r;}
	else if(sync_a == 'G'){start_audio = start_g;}
	else if(sync_a == 'B'){start_audio = start_b;}
	else if(red == 1 && green == 0 && blue == 0){start_audio = start_r; sync_a = 'R';}//select the channel if only 1 is activated
	else if(red == 0 && green == 1 && blue == 0){start_audio = start_g; sync_a = 'G';}
	else if(red == 0 && green == 0 && blue == 1){start_audio = start_b; sync_a = 'B';}
	else {start_audio = start_g; sync_a = 'G';}//default value
	
	if((pipe_mode != 'A') && (pipe_mode != 'R') && (pipe_mode != 'G') && (pipe_mode != 'B'))
	{
		fprintf(stderr, "\nUnknow parametter '%c' for option -pipeMode / value : (A,a,R,r,G,g,B,b)\n\n",pipe_mode);
		usage();
	}
	
	if((cmb_mode_r < 0 || cmb_mode_r > 2) || (cmb_mode_g < 0 ||cmb_mode_g > 2) || (cmb_mode_b < 0 || cmb_mode_b > 2))
	{
		fprintf(stderr, "\nCombine mode invalid / value : (0 ,1 ,2)\n\n");
		usage();
	}
	
	if((c_gain_r < 0 || c_gain_r > 6) || (c_gain_g < 0 || c_gain_g > 6) || (c_gain_b < 0 || c_gain_b > 6))
	{
		fprintf(stderr, "\nOne chroma gain is invalid / range : (0.0 ,4.0)\n\n");
		usage();
	}
	
	if((signal_gain_r < 0.5 || signal_gain_r > 2) || (signal_gain_g < 0.5 || signal_gain_g > 2) || (signal_gain_b < 0.5 || signal_gain_b > 2))
	{
		fprintf(stderr, "\nOne signal gain is invalid / range : (0.0 ,4.0)\n\n");
		usage();
	}
	
	if((ire_r < -50 || ire_r > 50) || (ire_g < -50 || ire_g > 50) || (ire_b < -50 || ire_b > 50))
	{
		fprintf(stderr, "\nIRE level is invalid / range : (-50.0 , 50.0)\n\n");
		usage();
	}
	
	if(((v_max_r < 0.003 || v_max_r > 0.7) && v_max_r != -1)|| ((v_max_g < 0.003 || v_max_g > 0.7) && v_max_g != -1) || ((v_max_b < 0.003 || v_max_b > 0.7) && v_max_b != -1))
	{
		fprintf(stderr, "\n maximum voltage (%f) is invalid / range : (0.003 , 0.7)\n\n",v_max_g);
		usage();
	}
	
	if((max_value_r < 1 || max_value_r > 255) || (max_value_g < 1 || max_value_g > 255) || (max_value_b < 1 || max_value_b > 255))
	{
		fprintf(stderr, "\n max value is invalid / range : (1 , 255)\n\n");
		usage();
	}
	
	if(v_max_r > 0.0)
	{
		fprintf(stderr, "\nGain and ire control disabled for output R\n\n");
		c_gain_r = 1;
		signal_gain_r = 1;
		ire_r = 0;
	}
	
	if(v_max_g > 0.0)
	{
		fprintf(stderr, "\nGain and ire control disabled for output G\n\n");
		c_gain_g = 1;
		signal_gain_g = 1;
		ire_g = 0;
	}
	
	if(v_max_b > 0.0)
	{
		fprintf(stderr, "\nGain and ire control disabled for output B\n\n");
		c_gain_b = 1;
		signal_gain_b = 1;
		ire_b = 0;
	}

	if(red == 1)
	{
		if(!strcmp(get_filename_ext(filename_r),"tbc")){r16 = 1;r_sign = 0;tbcR = 1;}
		else if(!strcmp(get_filename_ext(filename_r),"s8")){r16 = 0;r_sign = 1;}
		else if(!strcmp(get_filename_ext(filename_r),"u8")){r16 = 0;r_sign = 0;}
		else if(!strcmp(get_filename_ext(filename_r),"s16")){r16 = 1;r_sign = 1;}
		else if(!strcmp(get_filename_ext(filename_r),"u16")){r16 = 1;r_sign = 0;}
		else if(!strcmp(get_filename_ext(filename_r),"wav")){r16 = 1;r_sign = 1;}
		else if(!strcmp(get_filename_ext(filename_r),"pcm")){r16 = 1;r_sign = 1;}
		else if(!strcmp(get_filename_ext(filename_r),"efm")){r16 = 0;r_sign = 0;}
		else if(!strcmp(get_filename_ext(filename_r),"raw")){r16 = 1;r_sign = 1;}
		else if(!strcmp(get_filename_ext(filename_r),"r8")){r16 = 0;r_sign = 0;}
		else if(!strcmp(get_filename_ext(filename_r),"r16")){r16 = 1;r_sign = 0;}
		//else if(!strcmp(get_filename_ext(filename_r),"cds")){r16 = 1;r_sign = 0;}//10 bit packed
	}	
	if(green == 1)
	{
		if(!strcmp(get_filename_ext(filename_g),"tbc")){g16 = 1;g_sign = 0;tbcG = 1;}
		else if(!strcmp(get_filename_ext(filename_g),"s8")){g16 = 0;g_sign = 1;}
		else if(!strcmp(get_filename_ext(filename_g),"u8")){g16 = 0;g_sign = 0;}
		else if(!strcmp(get_filename_ext(filename_g),"s16")){g16 = 1;g_sign = 1;}
		else if(!strcmp(get_filename_ext(filename_g),"u16")){g16 = 1;g_sign = 0;}
		else if(!strcmp(get_filename_ext(filename_g),"wav")){g16 = 1;g_sign = 1;}
		else if(!strcmp(get_filename_ext(filename_g),"pcm")){g16 = 1;g_sign = 1;}
		else if(!strcmp(get_filename_ext(filename_g),"efm")){g16 = 0;g_sign = 0;}
		else if(!strcmp(get_filename_ext(filename_g),"raw")){g16 = 1;g_sign = 1;}
		else if(!strcmp(get_filename_ext(filename_g),"r8")){g16 = 0;g_sign = 0;}
		else if(!strcmp(get_filename_ext(filename_g),"r16")){g16 = 1;g_sign = 0;}
		//else if(!strcmp(get_filename_ext(filename_g),"cds")){g16 = 1;g_sign = 0;}
	}
	if(blue == 1)
	{
		if(!strcmp(get_filename_ext(filename_b),"tbc")){b16 = 1;b_sign = 0;tbcB = 1;}
		else if(!strcmp(get_filename_ext(filename_b),"s8")){b16 = 0;b_sign = 1;}
		else if(!strcmp(get_filename_ext(filename_b),"u8")){b16 = 0;b_sign = 0;}
		else if(!strcmp(get_filename_ext(filename_b),"s16")){b16 = 1;b_sign = 1;}
		else if(!strcmp(get_filename_ext(filename_b),"u16")){b16 = 1;b_sign = 0;}
		else if(!strcmp(get_filename_ext(filename_b),"wav")){b16 = 1;b_sign = 1;}
		else if(!strcmp(get_filename_ext(filename_b),"pcm")){b16 = 1;b_sign = 1;}
		else if(!strcmp(get_filename_ext(filename_b),"efm")){b16 = 0;b_sign = 0;}
		else if(!strcmp(get_filename_ext(filename_b),"raw")){b16 = 1;b_sign = 1;}
		else if(!strcmp(get_filename_ext(filename_b),"r8")){b16 = 0;b_sign = 0;}
		else if(!strcmp(get_filename_ext(filename_b),"r16")){b16 = 1;b_sign = 0;}
		//else if(!strcmp(get_filename_ext(filename_b),"cds")){b16 = 1;b_sign = 0;}
	}
	
	//16bit override
	if(override_r16 != -1){r16 = override_r16;}
	if(override_g16 != -1){g16 = override_g16;}
	if(override_b16 != -1){b16 = override_b16;}

	//tbc override
	if(override_tbc_r != -1){tbcR = override_tbc_r;}
	if(override_tbc_g != -1){tbcG = override_tbc_g;}
	if(override_tbc_b != -1){tbcB = override_tbc_b;}

	//sign override
	if(override_r_sign != -1){r_sign = override_r_sign;}
	if(override_g_sign != -1){g_sign = override_g_sign;}
	if(override_b_sign != -1){b_sign = override_b_sign;}
	
	if(input_sample_rate == 17734475 || input_sample_rate == 17735845)//PAL
	{
		start_r = start_r * ((709375 + (1135 * tbcR)) * (1 + r16));// set first frame
		start_g = start_g * ((709375 + (1135 * tbcB)) * (1 + g16));
		start_b = start_b * ((709375 + (1135 * tbcG)) * (1 + b16));
		start_audio = (start_audio + audio_offset) * ((88200/25) * 2);
		video_standard = 'P';
	}
	else if(input_sample_rate == 14318181 || input_sample_rate == 14318170)//NTSC
	{
		start_r = start_r * ((477750 + (910 * tbcR)) * (1 + r16));//set first frame
		start_g = start_g * ((477750 + (910 * tbcG)) * (1 + g16));
		start_b = start_b * ((477750 + (910 * tbcB)) * (1 + b16));
		start_audio = (start_audio + audio_offset) * ((88200/30) * 2);
		video_standard = 'N';
	}

//FL2K initialisation
	fl2k_open(&dev, (uint32_t)dev_index);
	if (NULL == dev) {
		fprintf(stderr, "Failed to open fl2k device #%d.\n", dev_index);
		goto out;
	}
	
	/* Set the sample rate */
	r = fl2k_set_sample_rate(dev, input_sample_rate);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to set sample rate.\n");

//file and buffer initialisation
	output_sample_rate = fl2k_get_sample_rate(dev);
	fprintf(stderr, "output sample rate = %d\n",output_sample_rate);

	if(resample)
	{
		input_buf_size = round((FL2K_BUF_LEN / ((double)output_sample_rate / (double)input_sample_rate))+ 0.5);
		
		//change to signed for resampling
		//and reverse output sign
		if(r_sign == 0) sample_type_r = !sample_type_r;
		if(g_sign == 0) sample_type_g = !sample_type_g;
		if(b_sign == 0) sample_type_b = !sample_type_b;
		r_sign = 1;
		g_sign = 1;
		b_sign = 1;
	}
	else
	{
		input_buf_size = FL2K_BUF_LEN;
	}
	fprintf(stderr, "set input_buf_size to : %d\n",input_buf_size);

//RED file
if(red == 1)
{
	if (strcmp(filename_r, "-") == 0)/* Read samples from stdin */
	{
		file_r = stdin;
	}
	else
	{
		file_r = fopen(filename_r, "rb");
		if (!file_r) {
			fprintf(stderr, "(RED) : Failed to open %s\n", filename_r);
			goto out;
		}
		else
		{
			FSEEK(file_r,start_r,0);
		}
	}

	inbuf_r = malloc(input_buf_size);
	resbuf_r = malloc(input_buf_size*2);
	outbuf_r = malloc(FL2K_BUF_LEN);
	if (!inbuf_r || !resbuf_r || !outbuf_r) {
		fprintf(stderr, "(RED) : malloc error!\n");
		goto out;
	}
	
}

if(red2 == 1)
{
	if (strcmp(filename2_r, "-") == 0)/* Read samples from stdin */
	{ 
		file2_r = stdin;
	}
	else 
	{
		file2_r = fopen(filename2_r, "rb");
		if (!file2_r) {
			fprintf(stderr, "(RED) : Failed to open %s\n", filename2_r);
			goto out;
		}
		else
		{
			FSEEK(file2_r,start_r,0);
		}
	}
}

//GREEN file
if(green == 1)
{
	if (strcmp(filename_g, "-") == 0)/* Read samples from stdin */
	{
		file_g = stdin;
	}
	else
	{
		file_g = fopen(filename_g, "rb");
		if (!file_g) {
			fprintf(stderr, "(GREEN) : Failed to open %s\n", filename_g);
			goto out;
		}
		else
		{
			FSEEK(file_g,start_g,0);
		}
	}

	inbuf_g = malloc(input_buf_size);
	resbuf_g = malloc(input_buf_size*2);
	outbuf_g = malloc(FL2K_BUF_LEN);
	if (!inbuf_g || !resbuf_g || !outbuf_g) {
		fprintf(stderr, "(GREEN) : malloc error!\n");
		goto out;
	}
}

if(green2 == 1)
{
	if (strcmp(filename2_g, "-") == 0)/* Read samples from stdin */
	{ 
		file2_g = stdin;
	}
	else 
	{
		file2_g = fopen(filename2_g, "rb");
		if (!file2_g) {
			fprintf(stderr, "(GREEN) : Failed to open %s\n", filename2_g);
			goto out;
		}
		else
		{
			FSEEK(file2_g,start_g,0);
		}
	}
}

//BLUE file
if(blue == 1)
{
	if (strcmp(filename_b, "-") == 0)/* Read samples from stdin */
	{
		file_b = stdin;
	}
	else
	{
		file_b = fopen(filename_b, "rb");
		if (!file_b) {
			fprintf(stderr, "(BLUE) : Failed to open %s\n", filename_b);
			goto out;
		}
		else
		{
			FSEEK(file_b,start_b,0);
		}
	}

	inbuf_b = malloc(input_buf_size);
	resbuf_b = malloc(input_buf_size*2);
	outbuf_b = malloc(FL2K_BUF_LEN);
	if (!inbuf_b || !resbuf_b || !outbuf_b) {
		fprintf(stderr, "(BLUE) : malloc error!\n");
		goto out;
	}
}

if(blue2 == 1)
{
	if (strcmp(filename2_b, "-") == 0)/* Read samples from stdin */
	{ 
		file2_b = stdin;
	}
	else 
	{
		file2_b = fopen(filename2_b, "rb");
		if (!file2_b) {
			fprintf(stderr, "(BLUE) : Failed to open %s\n", filename2_b);
			goto out;
		}
		else
		{
			FSEEK(file2_b,start_b,0);
		}
	}
}

if(audio == 1)
{
	if (strcmp(filename_audio, "-") == 0)/* Read samples from stdin */
	{ 
		file_audio = stdin;
	}
	else 
	{
		file_audio = fopen(filename_audio, "rb");
		if (!file_audio) {
			fprintf(stderr, "(AUDIO) : Failed to open %s\n", filename_audio);
			goto out;
		}
		else
		{
			FSEEK(file_audio,start_audio,0);
		}
	}
}

//start fl2K
r = fl2k_start_tx(dev, fl2k_callback, NULL, 0);


#ifndef _WIN32
	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigign.sa_handler = SIG_IGN;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigign, NULL);
#else
	SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
#endif

	while (!do_exit)
		sleep_ms(500);

	fl2k_close(dev);

out:

//close resampler
	if(resampler_r && red == 1)
	{
		resampler_close(resampler_r);
	}
	
	if(resampler_g && green == 1)
	{
		resampler_close(resampler_r);
	}
	
	if(resampler_b && blue == 1)
	{
		resampler_close(resampler_r);
	}

//RED
	if(red == 1)
	{
		if(inbuf_r)
		{
			free(inbuf_r);
		}
		
		if (resbuf_r)
		{
			free(resbuf_r);
		}
		
		if(outbuf_r)
		{
			free(outbuf_r);
		}

		if (file_r && (file_r != stdin))
		{
			fclose(file_r);
		}
	}
	
	if(red2 == 1)
	{
		if (file2_r && (file2_r != stdin))
		{
			fclose(file2_r);
		}
	}
	//GREEN
	if(green == 1)
	{
		if (inbuf_g)
		{
			free(inbuf_g);
		}
		
		if (resbuf_g)
		{
			free(resbuf_g);
		}
		
		if(outbuf_g)
		{
			free(outbuf_g);
		}

		if (file_g && (file_g != stdin))
		{
			fclose(file_g);
		}
	}
	
	if(green2 == 1)
	{
		if (file2_g && (file2_g != stdin))
		{
			fclose(file2_g);
		}
	}
	
	//BLUE	
	if(blue == 1)
	{
		if (inbuf_b)
		{
			free(inbuf_b);
		}
		
		if (resbuf_b)
		{
			free(resbuf_b);
		}
		
		if(outbuf_b)
		{
			free(outbuf_b);
		}

		if (file_b && (file_b != stdin))
		{
			fclose(file_b);
		}
	}
	
	if(blue2 == 1)
	{
		if (file2_b && (file2_b != stdin))
		{
			fclose(file2_b);
		}
	}
	
	if(audio == 1)
	{
		if (file_audio && (file_audio != stdin))
		{
			fclose(file_audio);
		}
	}
	
	//free(pipe_buf);

	return 0;
}
