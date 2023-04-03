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

static volatile int do_exit = 0;
static volatile int repeat = 1;

uint32_t samp_rate = 100000000;

//buffer for tx
char *txbuf_r = NULL;
char *txbuf_g = NULL;
char *txbuf_b = NULL;

//chanel activation
int red = 0;
int green = 0;
int blue = 0;

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

//pipe mode
char pipe_mode = 'G';

uint32_t sample_cnt_r = 0;
uint32_t sample_cnt_g = 0;
uint32_t sample_cnt_b = 0;

uint32_t line_cnt_r = 0;
uint32_t line_cnt_g = 0;
uint32_t line_cnt_b = 0;

//thread for processing
pthread_t thread_r;
pthread_t thread_g;
pthread_t thread_b;

unsigned char DC_R = 0;
unsigned char DC_G = 0; 
unsigned char DC_B = 0; 

void usage(void)
{
	fprintf(stderr,
		"fl2k_file2, a sample player for FL2K VGA dongles\n\n"
		"Usage:\n"
		"\t[-d device_index (default: 0)]\n"
		"\t[-s samplerate (default: 100 MS/s) you can write(ntsc) or (pal)]\n"
		"\t[-u Set the output sample type of the fl2K to unsigned]\n"
		"\t[-R write a 8bit value on channel R (0-255)\n"
		"\t[-G write a 8bit value on channel G (0-255)\n"
		"\t[-B write a 8bit value on channel B (0-255)\n"
		"\t[-Pipe chose the channel to copy on the output pipe (R, G, B)\n"
		"\t[-signPipe set pipe output as (1 = signed / 0 = unsigned) or (s = signed / u = unsigned)\n"
		"\t[-VmaxR maximum output voltage for channel R (0.003 to 0.7) (scale value) (disable Cgain and Sgain)\n"
		"\t[-VmaxG maximum output voltage for channel G (0.003 to 0.7) (scale value) (disable Cgain and Sgain)\n"
		"\t[-VmaxB maximum output voltage for channel B (0.003 to 0.7) (scale value) (disable Cgain and Sgain)\n"
		"\t[-MaxValueR max value for channel R (1 to 255) (reference level) (used for Vmax)\n"
		"\t[-MaxValueG max value for channel G (1 to 255) (reference level) (used for Vmax)\n"
		"\t[-MaxValueB max value for channel B (1 to 255) (reference level) (used for Vmax)\n"
		//"\t[-MinValueR min value for channel R (0 to 254) (reference level) (used for Vmax)\n"
		//"\t[-MinValueG min value for channel G (0 to 254) (reference level) (used for Vmax)\n"
		//"\t[-MinValueB min value for channel B (0 to 254) (reference level) (used for Vmax)\n"
	);
	exit(1);
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

void vbi_generator(long *sample_count,int *line_count,int is_60hz,long number_sample,unsigned char *buf)
{
	long i = 0;
	long frame_lengt;
	int line_lengt;
	int frame_nb_line;
	int v_start;
	int v_end;
	int cbust_start;
	int cbust_end;
	int sync_levels;
	int reference_black;
	int sample_in_line;
	
	if(is_60hz == 1)//ntsc (525)
	{
		frame_lengt = 477750;
		line_lengt = 910;
		frame_nb_line = 525;
		v_start = 134;
		v_end = 894;
		cbust_start = 78 ;
		cbust_end = 110;
		sync_levels = 3;
		reference_black = 60;
	}
	else//pal (625)
	{
		frame_lengt = 709375;
		line_lengt = 1135;
		frame_nb_line = 625;
		v_start = 185;
		v_end = 1107;
		cbust_start = 98;
		cbust_end = 138;
		sync_levels = 3;//not verified
		reference_black = 60;//not verified
	}
	
	while(i < number_sample)
	{
		sample_in_line = sample_count - (line_lengt * *line_count);
		buf[i] = reference_black;

		if(is_60hz == 1)//ntsc (525)
		{
			if((*line_count > 3 && *line_count < 7) && ((sample_in_line < 390) || (sample_in_line > 457 && sample_in_line < 845)))
			{
				buf[i] = sync_levels;
			}
			if((*line_count > 266 && *line_count < 270) && (sample_in_line < 390 ))
			{
				buf[i] = sync_levels;
			}
			if((*line_count > 265 && *line_count < 269) && ((sample_in_line > 457) && (sample_in_line < 845)))
			{
				buf[i] = sync_levels;
			}
			if((*line_count < 10) && ((sample_in_line > 457) && (sample_in_line < 491)))
			{
				buf[i] = sync_levels;
			}			
			if(((*line_count < 262) && (*line_count < 272)) && ((sample_in_line > 457) && (sample_in_line < 491)))
			{
				buf[i] = sync_levels;
			}
			if(sample_in_line < 60 && ((*line_count > 9 && *line_count < 264) || (*line_count > 272)))
			{
				buf[i] = sync_levels;
			}
			if(sample_in_line < 35)
			{
				buf[i] = sync_levels;
			}
		}
		
		i++;
		sample_count++;
		
		if(*sample_count > frame_lengt)
		{
			*sample_count = 0;
			*line_count = 0;
		}
	}
}

int read_sample_file(void *inpt_color)
{
	//parametter
	void *buffer = NULL;
	char color = (char *) inpt_color;
	double v_max = -1;
	int max_value = 255;
	unsigned char DC;
	
	//pipe
	int use_pipe = 0;
	
	long i = 0;//counter for tmp_buf
	long y = 0;//counter for calc
	
	//count
	uint32_t *sample_cnt = NULL;
	int *line_cnt = NULL;

	if(color == 'R')
	{
		buffer = txbuf_r;
		sample_cnt = &sample_cnt_r;
		line_cnt = &line_cnt_r;
		v_max = v_max_r;
		max_value = max_value_r;
		DC = DC_R;
		if(pipe_mode == 'R')
		{
			use_pipe = 1;
		}
	}
	else if(color == 'G')
	{
		buffer = txbuf_g;
		sample_cnt = &sample_cnt_g;
		line_cnt = &line_cnt_g;
		v_max = v_max_g;
		max_value = max_value_g;
		DC = DC_G;
		if(pipe_mode == 'G')
		{
			use_pipe = 1;
		}
	}
	else if(color == 'B')
	{
		buffer = txbuf_b;
		sample_cnt = &sample_cnt_b;
		line_cnt = &line_cnt_b;
		v_max = v_max_b;
		max_value = max_value_b;
		DC = DC_B;
		if(pipe_mode == 'B')
		{
			use_pipe = 1;
		}
	}
	
	unsigned long buf_size = 1310720;
	
	unsigned char *tmp_buf = malloc(1310720);
	unsigned char *calc = malloc(buf_size);
	
	if (tmp_buf == NULL || calc == NULL)
	{
		free(tmp_buf);   // Free both in case only one was allocated
		free(calc);
		fprintf(stderr, "(%c) malloc error (tmp_buf , calc)\n",color);
		return -1;
	}
	
	vbi_generator(sample_cnt,line_cnt,1,1310720,tmp_buf);

	/*while((y < buf_size) && !do_exit)
	{*/
		//if we are at then end of the frame skip one line
		/*if(*sample_cnt == frame_lengt)//wave length
		{
			*sample_cnt = 0;
			
			//write audio file to stdout only if its not a terminal
			if(isatty(STDOUT_FILENO) == 0 && is_sync_a)
			{
				//write(stdout, tmp_buf, 1310720);
				fread(audio_buf_signed,audio_frame,1,streamA);
				//write(stdout, audio_buf_signed, audio_frame);
				fwrite(audio_buf, audio_frame,1,stdout);
				fflush(stdout);
			}
		}*/
		
		//scale to max voltage
		/*if(v_max > 0.0)
		{
			if(round(tmp_buf[i]*(255/max_value)) > 255)
			{
				tmp_buf[i] = 255;
			}
			else
			{
				tmp_buf[i] = round((tmp_buf[i]*(255/max_value))/(0.7/v_max));
			}
		}*/
		/*tmp_buf[i] = DC;
		fprintf(stderr, "DC value : %d.\n",tmp_buf[i]);
		i += 1;//on avance tmp_buff de 1
		y += 1;
		
		*sample_cnt += 1;
	}*/
	
	memcpy(buffer, tmp_buf, 1310720);
	if(isatty(STDOUT_FILENO) == 0 && use_pipe)
	{
		fwrite(tmp_buf, 1310720,1,stdout);
		fflush(stdout);
	}
	
	free(tmp_buf);
	free(calc);
	return 0;
}

void fl2k_callback(fl2k_data_info_t *data_info)
{	
	static uint32_t repeat_cnt = 0;

	if (data_info->device_error) {
		fprintf(stderr, "Device error, exiting.\n");
		do_exit = 1;
		return;
	}
	
	//set sign (signed = 1 , unsigned = 0)
	data_info->sampletype_signed_r = 0;
	data_info->sampletype_signed_g = 0;
	data_info->sampletype_signed_b = 0;
	
	//send the bufer with a size of 1310720
	if(red == 1)
	{
		data_info->r_buf = txbuf_r;
	}
	if(green == 1)
	{
		data_info->g_buf = txbuf_g;
	}
	if(blue == 1)
	{
		data_info->b_buf = txbuf_b;
	}

	//RED
	if(red == 1)
	{
		pthread_create(&thread_r,NULL,read_sample_file,'R');
	}
	
	//GREEN
	if(green == 1)
	{
		pthread_create(&thread_g,NULL,read_sample_file,'G');
	}
	
	//BLUE
	if(blue == 1)
	{
		pthread_create(&thread_b,NULL,read_sample_file,'B');
	}
	
	if(red == 1)
	{
		pthread_join(thread_r,NULL);
		pthread_exit(thread_r);
	}
	if(green == 1)
	{
		pthread_join(thread_g,NULL);
		pthread_exit(thread_g);
	}
	if(blue == 1)
	{
		pthread_join(thread_b,NULL);
		pthread_exit(thread_b);
	}
	//fl2k_stop_tx(dev);
	//do_exit = 1;
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
	
	int option_index = 0;
	static struct option long_options[] = {
		{"DC_R", 1, 0, 1},
		{"DC_G", 1, 0, 2},
		{"DC_B", 1, 0, 3},
		{"Pipe", 1, 0, 25},
		{"signPipe", 1, 0, 30},
		{"VmaxR", 1, 0, 38},
		{"VmaxG", 1, 0, 39},
		{"VmaxB", 1, 0, 40},
		{"MaxValueR", 1, 0, 41},
		{"MaxValueG", 1, 0, 42},
		{"MaxValueB", 1, 0, 43},
		{0, 0, 0, 0}//reminder : letter value are from 65 to 122
	};

	while ((opt = getopt_long_only(argc, argv, "d:r:s:R:G:B:A:",long_options, &option_index)) != -1) {
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
				samp_rate = (uint32_t) 14318181;
			}
			else if((strcmp(optarg, "pal" ) == 0) || (strcmp(optarg, "PAL" ) == 0) || (strcmp(optarg, "Pal" ) == 0))
			{
				samp_rate = (uint32_t) 17734475;
			}
			else
			{
				samp_rate = (uint32_t)atof(optarg);
			}
			break;
		case 1:
			DC_R = atof(optarg);
			red = 1;
			break;
		case 2:
			DC_G = atof(optarg);
			green = 1;
			break;
		case 3:
			DC_B = atof(optarg);
			blue = 1;
			break;
		case 25:
			if(*optarg == 'r'){pipe_mode = 'R';}
			else if(*optarg == 'g'){pipe_mode = 'G';}
			else if(*optarg == 'b'){pipe_mode = 'B';}
			else{pipe_mode = *optarg;}
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

	if(red == 0 && green == 0 && blue == 0)
	{
		fprintf(stderr, "\nNo option provided for any channel(-R,-G,-B)\n\n");
		usage();
	}
	
	if((pipe_mode != 'R') && (pipe_mode != 'G') && (pipe_mode != 'B'))
	{
		fprintf(stderr, "\nUnknow parametter '%c' for option -pipeMode / value : (R,r,G,g,B,b)\n\n",pipe_mode);
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
	
//RED file
if(red == 1)
{
	txbuf_r = malloc(FL2K_BUF_LEN);
	if (!txbuf_r) {
		fprintf(stderr, "(RED) : malloc error!\n");
		goto out;
	}
}

//GREEN file
if(green == 1)
{
	txbuf_g = malloc(FL2K_BUF_LEN);
	if (!txbuf_g) {
		fprintf(stderr, "(GREEN) : malloc error!\n");
		goto out;
	}
}

//BLUE file
if(blue == 1)
{
	txbuf_b = malloc(FL2K_BUF_LEN);
	if (!txbuf_b) {
		fprintf(stderr, "(BLUE) : malloc error!\n");
		goto out;
	}
}

//next

	fl2k_open(&dev, (uint32_t)dev_index);
	if (NULL == dev) {
		fprintf(stderr, "Failed to open fl2k device #%d.\n", dev_index);
		goto out;
	}

	r = fl2k_start_tx(dev, fl2k_callback, NULL, 0);

	/* Set the sample rate */
	r = fl2k_set_sample_rate(dev, samp_rate);
	if (r < 0)
		fprintf(stderr, "WARNING: Failed to set sample rate.\n");


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

//RED
	if(red == 1)
	{
		if (txbuf_r)
		{
			free(txbuf_r);
		}
	}
	//GREEN
	if(green == 1)
	{
		if (txbuf_g)
		{
			free(txbuf_g);
		}
	}
	
	//BLUE	
	if(blue == 1)
	{
		if (txbuf_b)
		{
			free(txbuf_b);
		}
	}

	return 0;
}
