/*
   io-latency-test.c

   By Philipp Reisner.

   Copyright (C) 2006, Philipp Reisner <philipp.reisner@linbit.com>.
        Initial author.

   io-latency-test is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   dm is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with dm; see the file COPYING.  If not, write to
   the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.

 */

/* In case this crashes (in your UML)
   touch /etc/ld.so.nohwcap
 */

// compile with gcc -pthread -o io-latency-test io-latency-test.c

#include <sys/poll.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <stdio.h>

#define MONITOR_TIME 300000
// Check every 300 milliseconds. (3.33 times per second)

#define RECORD_TIME 20000
// Try to write a record every 20 milliseconds (50 per second)

struct shared_data {
	pthread_mutex_t mutex;
	unsigned long record_nr;
	unsigned int write_duration_us;
	unsigned int write_duration_records;
};

void* wd_thread(void *arg)
{
	struct shared_data *data = (struct shared_data*) arg;
	unsigned long last_record_nr=-1, current_record_nr;
	unsigned int avg_write,wd,wr;
	enum { IO_RUNNING, IO_BLOCKED } io_state = IO_RUNNING;
	
	while(1) {
		usleep(MONITOR_TIME); // sleep some milliseconds

		pthread_mutex_lock(&data->mutex);
		current_record_nr = data->record_nr;
		wd = data->write_duration_us;
		wr = data->write_duration_records;
		data->write_duration_us = 0;
		data->write_duration_records = 0;
		pthread_mutex_unlock(&data->mutex);

		switch(io_state) {
		case IO_RUNNING:
			if(current_record_nr == last_record_nr) {
				printf("IO got frozen. Last completely "
				       "written record: %lu\n",
				       last_record_nr);
				io_state = IO_BLOCKED;
			} else {
				if(wr==0) wr=1;
				avg_write = wd/wr;

				printf("Current record: %lu "
				       "( AVG write duration %d.%02dms )\r",
				       current_record_nr,
				       avg_write/1000,(avg_write%1000)/10);
				fflush(stdout);
			}
			last_record_nr = current_record_nr;
		case IO_BLOCKED:
			if(current_record_nr != last_record_nr) {
				printf("IO just resumed.\n");
				io_state = IO_RUNNING;
			}
		}
	}
}

int main(int argc, char** argv)
{
	pthread_t watch_dog;
	unsigned long record_nr=0;
	FILE* record_f;

	struct timeval now_tv, then_tv;
	struct tm now_tm;
	int write_duration_us=0;

	struct shared_data data;

	if(argc != 2) {
		fprintf(stderr,"USAGE: %s recordfile\n",argv[0]);
		return 10;
	}

	if(!(record_f = fopen(argv[1],"w"))) {
		perror("fopen:");
		fprintf(stderr,"Failed to open '%s' for writing\n",argv[1]);
		return 10;
	}

	printf("\n"
	       "This programm writes records to a file, shows the write latency\n"
	       "of the file system and block device combination and informs\n"
	       "you in case IO completely stalls.\n\n"
	       "  Due to the nature of the 'D' process state on Linux\n"
	       "  (and other Unix operating systems) you can not kill this\n"
	       "  test programm while IO is frozen. You have to kill it with\n"
	       "  Ctrl-C (SIGINT) while IO is running.\n\n"
	       "In case the record file's block device freezes, this "
	       "program will\n"
	       "inform you here which record was completely written before it "
	       "freezed.\n\n"
	       );

	pthread_mutex_init(&data.mutex,NULL);
	data.record_nr = record_nr;
	data.write_duration_us = 0;
	data.write_duration_records = 1;
	pthread_create(&watch_dog,NULL,wd_thread,&data);

	for(;;record_nr++) {
		gettimeofday(&now_tv, NULL);
		localtime_r(&now_tv.tv_sec,&now_tm);

		fprintf(record_f,
			"%04d-%02d-%02d %02d:%02d:%02d.%06ld: "
			"Record number: %-6lu "
			"(L.r.w.t.: %d.%02dms)\n",
			1900+ now_tm.tm_year,
			1+ now_tm.tm_mon,
			now_tm.tm_mday,
			now_tm.tm_hour,
			now_tm.tm_min,
			now_tm.tm_sec,
			now_tv.tv_usec,
			record_nr,
			write_duration_us/1000,
			(write_duration_us%1000)/10);
		
		fflush(record_f); // flush it from glibc to the kernel.
		fdatasync(fileno(record_f)); // from buffer cache to disk.

		// eventually wait for full RECORD_TIME
		gettimeofday(&then_tv, NULL);
		write_duration_us =
			( (then_tv.tv_sec  - now_tv.tv_sec ) * 1000000 +
			  (then_tv.tv_usec - now_tv.tv_usec) );

		pthread_mutex_lock(&data.mutex);
		data.record_nr = record_nr;
		data.write_duration_us += write_duration_us;
		data.write_duration_records++;
		pthread_mutex_unlock(&data.mutex);

		if(write_duration_us < RECORD_TIME ) {
			usleep(RECORD_TIME - write_duration_us);
		}
	}
}
