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

#ifndef _WIN32
#include <unistd.h>
#define sleep_ms(ms)	usleep(ms*1000)
#else
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#define sleep_ms(ms)	Sleep(ms)
#endif

#include "osmo-fl2k.h"

static fl2k_dev_t *dev = NULL;

static volatile int do_exit = 0;
static volatile int repeat = 1;

uint32_t samp_rate = 100000000;

//input file
FILE *file_r;
FILE *file_g;
FILE *file_b;

//buffer for tx
char *txbuf_r = NULL;
char *txbuf_g = NULL;
char *txbuf_b = NULL;

//chanel activation
int red = 0;
int green = 0;
int blue = 0;

//enable 16 bit to 8 bit conversion
int r16 = 0;
int g16 = 0;
int b16 = 0;

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

//
int read_mode = 0;//0 = multitthreading / 1 = hybrid (R --> GB) / 2 = hybrid (RG --> B) / 3 = sequential (R -> G -> B)

int sample_type = 1;// 1 == signed   0 == unsigned

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

//thread for processing
pthread_t thread_r;
pthread_t thread_g;
pthread_t thread_b;

void usage(void)
{
	fprintf(stderr,
		"fl2k_file2, a sample player for FL2K VGA dongles\n\n"
		"Usage:\n"
		"\t[-d device_index (default: 0)]\n"
		"\t[-s samplerate (default: 100 MS/s) you can write(ntsc) or (pal)]\n"
		"\t[-u Set sample type to unsigned]\n"
		"\t[-R filename (use '-' to read from stdin)\n"
		"\t[-G filename (use '-' to read from stdin)\n"
		"\t[-B filename (use '-' to read from stdin)\n"
		"\t[-R16 (convert bits 16 to 8)\n"
		"\t[-G16 (convert bits 16 to 8)\n"
		"\t[-B16 (convert bits 16 to 8)\n"
		"\t[-tbcR interpret R as tbc file\n"
		"\t[-tbcG interpret G as tbc file\n"
		"\t[-tbcB interpret B as tbc file\n"
		"\t[-CgainR chroma gain for input R (0.0 to 6.0) (using color burst)\n"
		"\t[-CgainG chroma gain for input G (0.0 to 6.0) (using color burst)\n"
		"\t[-CgainB chroma gain for input B (0.0 to 6.0) (using color burst)\n"
		"\t[-ireR IRE level for input R (-50.0 to +50.0)\n"
		"\t[-ireG IRE level for input G (-50.0 to +50.0)\n"
		"\t[-ireB IRE level for input B (-50.0 to +50.0)\n"
		"\t[-readMode (default = 0) option : 0 = multit-threading (RGB) / 1 = hybrid (R --> GB) / 2 = hybrid (RG --> B) / 3 = sequential (R -> G -> B)\n"
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

//compute number of sample to skip
unsigned long calc_nb_skip(long sample_cnt,int linelength,long frame_lengt,long bufsize)
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
	return (nb_skip * linelength);//multiply for giving the number of byte to skip
}

int *read_sample_file(void *inpt_color)
{
	//parametter
	void *buffer = NULL;
	FILE *stream = NULL;
	int istbc = 0;
	char color = (char *) inpt_color;
	uint32_t sample_rate = samp_rate;
	double *chroma_gain = NULL;
	double *ire_level = NULL;
	
	int is16 = 0;
	
	long i = 0;//counter for tmp_buf
	long y = 0;//counter for calc
	
	//(NTSC line = 910 frame = 477750) (PAL line = 1135 frame = 709375)
	unsigned long frame_lengt = 0;
	unsigned long frame_nb_line = 0;
	unsigned int v_start =0;
	unsigned int v_end =0;
	unsigned long line_lengt = 0;
	unsigned long sample_skip = 0;
	
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
	
	if(color == 'R')
	{
		buffer = txbuf_r;
		stream = file_r;
		istbc = tbcR;
		sample_cnt = &sample_cnt_r;
		line_cnt = &line_cnt_r;
		line_sample_cnt = &line_sample_cnt_r;
		field_cnt = & field_cnt_r;
		chroma_gain = &c_gain_r;
		ire_level = &ire_r;
		if(r16 == 1)
		{
			is16 = 1;
		}
	}
	else if(color == 'G')
	{
		buffer = txbuf_g;
		stream = file_g;
		istbc = tbcG;
		sample_cnt = &sample_cnt_g;
		line_cnt = &line_cnt_g;
		line_sample_cnt = &line_sample_cnt_g;
		field_cnt = & field_cnt_g;
		chroma_gain = &c_gain_g;
		ire_level = &ire_g;
		if(g16 == 1)
		{
			is16 = 1;
		}
	}
	else if(color == 'B')
	{
		buffer = txbuf_b;
		stream = file_b;
		istbc = tbcB;
		sample_cnt = &sample_cnt_b;
		line_cnt = &line_cnt_b;
		line_sample_cnt = &line_sample_cnt_b;
		field_cnt = & field_cnt_b;
		chroma_gain = &c_gain_b;
		ire_level = &ire_b;
		if(b16 == 1)
		{
			is16 = 1;
		}
	}
	
	//IRE
	const float ire_conv = 1.59375;// (255/160)
	const float ire_min = 63.75;//40 * (255/160)
	const float ire_new_max = 159.375;// (140 * (255/160)) - (40 * (255/160))
	const float ire_add = (*ire_level * ire_conv);
	const float ire_gain = (ire_new_max / (ire_new_max + ire_add));
	double ire_tmp = 0;
	
	if(sample_rate == 17734475 || sample_rate == 17735845)//PAL multiplied by 2 if input is 16bit
	{
		frame_lengt = 709375 * (1 + is16);
		line_lengt = 1135 * (1 + is16);
		frame_nb_line = 625;
		v_start = 185 * (1 + is16);
		v_end = 1107 * (1 + is16);
		cbust_start = 98 * (1 + is16);//not set
		cbust_end = 138 * (1 + is16);//not set
	}
	else if(sample_rate == 14318181 || sample_rate == 14318170)//NTSC multiplied by 2 if input is 16bit
	{
		frame_lengt = 477750 * (1 + is16);
		line_lengt = 910 * (1 + is16);
		frame_nb_line = 525;
		v_start = 134 * (1 + is16);
		v_end = 894 * (1 + is16);
		cbust_start = 78 * (1 + is16);
		cbust_end = 110 * (1 + is16);
	}
	
	unsigned long buf_size = (1310720 + (is16 * 1310720));
	
	if(istbc == 1)
	{
		sample_skip = calc_nb_skip(*sample_cnt,line_lengt,frame_lengt,buf_size);
	}
	
	buf_size += sample_skip;
	
	unsigned char *tmp_buf = malloc(1310720);
	unsigned char *calc = malloc(buf_size);
	
	if (tmp_buf == NULL || calc == NULL)
	{
		free(tmp_buf);   // Free both in case only one was allocated
		free(calc);
		fprintf(stderr, "(%c) malloc error (tmp_buf , calc)\n",color);
		return -1;
	}
	
	if(istbc == 1)
	{
		sample_skip = calc_nb_skip(*sample_cnt,line_lengt,frame_lengt,buf_size);
	}
	
	if(fread(calc,buf_size,1,stream) != 1)
	{
		free(tmp_buf);   // Free both in case only one was allocated
		free(calc);
		fprintf(stderr, "(%c) fread error %d : ",color,errno);
		perror(NULL);
		return -1;
	}
	
	while((y < buf_size) && !do_exit)
	{
		//if we are at then end of the frame skip one line
		if((*sample_cnt == frame_lengt) && (istbc == 1))
		{
			//skip 1 line
			y += line_lengt;
			*sample_cnt = 0;
		}
		
		if(is16 == 1)
		{
			tmp_buf[i] = round(((calc[y+1] * 256) + calc[y])/ 256.0);//convert to 8 bit
			y += 2;
		}
		else
		{
			tmp_buf[i] = calc[y];
			y += 1;
		}
		
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
				
				if(*ire_level > 0)//if we add ire
				{
					ire_tmp += ire_add;
					tmp_buf[i] = round((ire_tmp * ire_gain) + ire_min);
				}
				else if(*ire_level < 0)//if we remove ire
				{
					ire_tmp = ire_tmp * ire_gain;
					tmp_buf[i] += round(ire_add + ire_min);
				}
			}
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
	
	memcpy(buffer, tmp_buf, 1310720);
	
	free(tmp_buf);
	free(calc);
	
	if(color == 'R')
	{
		pthread_exit(thread_r);
	}
	else if(color == 'G')
	{
		pthread_exit(thread_g);
	}
	else if(color == 'B')
	{
		pthread_exit(thread_b);
	}
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
	data_info->sampletype_signed = sample_type;
	
	//send the bufer with a size of 1310720
	if(red == 1)
	{
		data_info->r_buf = txbuf_r;
	}
	if(green == 1)
	{
		//printf("Valeur : %d %u %x\n",*txbuf_g,*txbuf_g,*txbuf_g);
		data_info->g_buf = txbuf_g;
	}
	if(blue == 1)
	{
		data_info->b_buf = txbuf_b;
	}

	//read until buffer is full
	//RED
	if(red == 1 && !feof(file_r))
	{
		pthread_create(&thread_r,NULL,read_sample_file,'R');
		
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
		}
	}
	
	//GREEN
	if(green == 1 && !feof(file_g))
	{
		pthread_create(&thread_g,NULL,read_sample_file,'G');
		
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
		}
	}
	else if(read_mode == 2)
	{
		if(red == 1)
		{
			pthread_join(thread_r,NULL);
		}
		if(green == 1)
		{
			pthread_join(thread_g,NULL);
		}
	}
	
	//BLUE
	if(blue == 1 && !feof(file_b))
	{
		pthread_create(&thread_b,NULL,read_sample_file,'B');
		
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
		}
		if(green == 1)
		{
			pthread_join(thread_g,NULL);
		}
		if(blue == 1)
		{
			pthread_join(thread_b,NULL);
		}
	}
	else if(read_mode == 3 || read_mode == 2)
	{
		if(blue == 1)
		{
			pthread_join(thread_b,NULL);
		}
	}
	else if(read_mode == 1)
	{
		if(green == 1)
		{
			pthread_join(thread_g,NULL);
		}
		if(blue == 1)
		{
			pthread_join(thread_b,NULL);
		}
	}
	
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
	int r, opt, i;
	uint32_t buf_num = 0;
	int dev_index = 0;
	void *status;

	//file adress
	char *filename_r = NULL;
	char *filename_g = NULL;
	char *filename_b = NULL;
	
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
		{0, 0, 0, 0}
	};

	while ((opt = getopt_long_only(argc, argv, "d:r:s:uR:G:B:",long_options, &option_index)) != -1) {
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
		case 'u':
			sample_type = 0;
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
		case 1:
			r16 = 1;
			break;
		case 2:
			g16 = 1;
			break;
		case 3:
			b16 = 1;
			break;
		case 4:
			tbcR = 1;
			break;
		case 5:
			tbcG = 1;
			break;
		case 6:
			tbcB = 1;
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
	
	if((c_gain_r < 0 || c_gain_r > 6) || (c_gain_g < 0 || c_gain_g > 6) || (c_gain_b < 0 || c_gain_b > 6))
	{
		fprintf(stderr, "\nOne chroma gain is invalid range (0.0 ,4.0)\n\n");
		usage();
	}
	
	if((ire_r < -50 || ire_r > 50) || (ire_g < -50 || ire_g > 50) || (ire_b < -50 || ire_b > 50))
	{
		fprintf(stderr, "\nIRE level is invalid range : (-50.0 , 50.0)\n\n");
		usage();
	}
	
//RED
if(red == 1)
{
	if (strcmp(filename_r, "-") == 0) { /* Read samples from stdin */
		file_r = stdin;
#ifdef _WIN32
		_setmode(_fileno(stdin), _O_BINARY);
#endif
	} else {
		file_r = fopen(filename_r, "rb");
		if (!file_r) {
			fprintf(stderr, "(RED) : Failed to open %s\n", filename_r);
			return -ENOENT;
		}
	}

	txbuf_r = malloc(FL2K_BUF_LEN);
	if (!txbuf_r) {
		fprintf(stderr, "(RED) : malloc error!\n");
		goto out;
	}
}

//GREEN
if(green == 1)
{
	if (strcmp(filename_g, "-") == 0) { /* Read samples from stdin */
		file_g = stdin;
#ifdef _WIN32
		_setmode(_fileno(stdin), _O_BINARY);
#endif
	} else {
		file_g = fopen(filename_g, "rb");
		if (!file_g) {
			fprintf(stderr, "(GREEN) : Failed to open %s\n", filename_g);
			return -ENOENT;
		}
	}

	txbuf_g = malloc(FL2K_BUF_LEN);
	if (!txbuf_g) {
		fprintf(stderr, "(GREEN) : malloc error!\n");
		goto out;
	}
}
//BLUE
if(blue == 1)
{
	if (strcmp(filename_b, "-") == 0) { /* Read samples from stdin */
		file_b = stdin;
#ifdef _WIN32
		_setmode(_fileno(stdin), _O_BINARY);
#endif
	} else {
		file_b = fopen(filename_b, "rb");
		if (!file_b) {
			fprintf(stderr, "(BLUE) : Failed to open %s\n", filename_b);
			return -ENOENT;
		}
	}

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

	if (file_r && (file_r != stdin))
	{
		fclose(file_r);
	}
}
//GREEN
if(green == 1)
{
	if (txbuf_g)
	{
		free(txbuf_g);
	}

	if (file_g && (file_g != stdin))
	{
		fclose(file_g);
	}
}
//BLUE	
if(blue == 1)
{
	if (txbuf_b)
	{
		free(txbuf_b);
	}

	if (file_b && (file_b != stdin))
	{
		fclose(file_b);
	}
}

	return 0;
}
