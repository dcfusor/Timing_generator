/*
Using timerfd
The timerfd interface is a Linux-specific set of functions that 
present POSIX timers as file descriptors (hence the fd) rather than 
signals thus avoiding all that tedious messing about with signal 
handlers. 
It was first implemented in GNU libc 2.8 and kernel 2.6.25: 
if you have them I highly recommend this approach.

You create a timer by calling timerfd_create() giving the POSIX 
clock id CLOCK_REALTIME or CLOCK_MONOTONIC. 
For periodic timers such as we are creating it does not matter 
which you choose. For absolute timers the expiry time is changed 
if the system clock is changed and the clock is CLOCK_REALTIME. 
In almost all cases, CLOCK_MONOTONIC is the one to use. 
timerfd_create returns a file descriptor for the timer.

To set the timer running, call timerfd_settime() giving flag 
 = TFD_TIMER_ABSTIME for an absolute timer or 0 for 
relative, as we want here, and the period in seconds and nanoseconds. 
To wait for the timer to expire, read from its file descriptor. 
It always returns an unsigned long long (8 byte unsigned integer) 
representing the number of timer events since the last read, which 
should be one if all is going well. 
If it is more than one then some events have been missed. 
In my example below I keep a record in "wakeups_missed".

*/
 
/* 
 * Copyright (C) 2009 Chris Simmonds (chris@2net.co.uk)
 *
 * This is a demonstration of periodic threads using the Linux timerfd
 * interface which was introduced in GNU libc 2.8 and kernel 2.6.25.

 Munged around to create just one thread, and bang an io pin on a raspi 
 by Doug Coulter 9/26/2019
  BC2708 is pi3
  BC2711 is pi4, change as required
  note you have to explicitly link pthread in your build
  I started with this for timing:  
  http://www.2net.co.uk/tutorial/periodic_threads
  and this for pi IO: 
  https://www.raspberrypi.org/forums/viewtopic.php?t=244031
  See the github referenced from that thread to get some sample code
 */
//#include <ncurses.h>

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/timerfd.h>

#include <fcntl.h>
#include <sys/mman.h>
// for daemon
#include <signal.h>
#include <sys/stat.h>
#include <syslog.h>

// for socket
#include <unistd.h>  // for usleep
#include <sys/socket.h> 
#include <sys/types.h> 
#include <arpa/inet.h> 
#include <netinet/in.h> 

// IO Access from ARM Running Linux

#define BCM2708_PERI_BASE        0x3F000000
#define BCM2711_PERI_BASE        0xFE000000
#define GPIO_BASE                (BCM2711_PERI_BASE + 0x200000) /* GPIO controller */
#define PAGE_SIZE (4*1024)
#define BLOCK_SIZE (4*1024)

// GPIO setup macros. Always use INP_GPIO(x) before using OUT_GPIO(x) or SET_GPIO_ALT(x,y)
#define INP_GPIO(g) *(gpio+((g)/10)) &= ~(7<<(((g)%10)*3))
#define OUT_GPIO(g) *(gpio+((g)/10)) |=  (1<<(((g)%10)*3))
#define SET_GPIO_ALT(g,a) *(gpio+(((g)/10))) |= (((a)<=3?(a)+4:(a)==4?3:2)<<(((g)%10)*3))

#define GPIO_SET *(gpio+7)  // sets   bits which are 1 ignores bits which are 0
#define GPIO_CLR *(gpio+10) // clears bits which are 1 ignores bits which are 0

#define GET_GPIO(g) (*(gpio+13)&(1<<g)) // 0 if LOW, (1<<g) if HIGH

#define GPIO_PULL *(gpio+37) // Pull up/pull down
#define GPIO_PULLCLK0 *(gpio+38) // Pull up/pull down clock

////////////////////////////
#define STROBE_PIN 21
#define PORT	 42742 
#define MAXLINE 1024 

////////////////////////////////////////////////////////////////////////////////////////////////////
// I/O access
volatile unsigned *gpio;
int  mem_fd;
void *gpio_map;
unsigned int rep;

////////////////////////////////////////////////////////////////////////////////////////////////////
struct periodic_info {
	int timer_fd;
	unsigned long long wakeups_missed;
};
static int thread_1_count;

volatile int run_state;
int debug = 0;
///////////////////////////////////////////////////////////////////////
static void skeleton_daemon()
{
    pid_t pid;

    /* Fork off the parent process */
    pid = fork();

    /* An error occurred */
    if (pid < 0)
        exit(EXIT_FAILURE);

    /* Success: Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* On success: The child process becomes session leader */
    if (setsid() < 0)
        exit(EXIT_FAILURE);

    /* Catch, ignore and handle signals */
    //TODO: Implement a working signal handler */
//    signal(SIGCHLD, SIG_IGN);
//    signal(SIGHUP, SIG_IGN);

    /* Fork off for the second time*/
    pid = fork();

    /* An error occurred */
    if (pid < 0)
        exit(EXIT_FAILURE);

    /* Success: Let the parent terminate */
    if (pid > 0)
        exit(EXIT_SUCCESS);

    /* Set new file permissions */
    umask(0);

    /* Change the working directory to the root directory */
    /* or another appropriated directory */
    chdir("/");

    /* Close all open file descriptors */
    int x;
    for (x = sysconf(_SC_OPEN_MAX); x>=0; x--)
    {
        close (x);
    }

    /* Open the log file */
    openlog ("strobe_daemon", LOG_PID, LOG_DAEMON);
}

/////////////////////////////
static int make_periodic(unsigned int period, struct periodic_info *info)
{
	int ret;
	unsigned int ns;
	unsigned int sec;
	int fd;
	struct itimerspec itval;

	/* Create the timer */
	fd = timerfd_create(CLOCK_MONOTONIC, 0);
	info->wakeups_missed = 0;
	info->timer_fd = fd;
	if (fd == -1)
		return fd;

	/* Make the timer periodic */
	sec = period / 1000000;
	ns = (period - (sec * 1000000)) * 1000;
	itval.it_interval.tv_sec = sec;
	itval.it_interval.tv_nsec = ns;
	itval.it_value.tv_sec = sec;
	itval.it_value.tv_nsec = ns;
	ret = timerfd_settime(fd, 0, &itval, NULL);
	return ret;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
static void wait_period(struct periodic_info *info)
{
	unsigned long long missed;
	int ret;

	/* Wait for the next timer event. If we have missed any the
	   number is written to "missed" */
	ret = read(info->timer_fd, &missed, sizeof(missed));
	if (ret == -1) {
		if (debug) perror("read timer");
		return;
	}

	info->wakeups_missed += missed;
}
////////////////////////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////////////////////////
static void *thread_1(void *arg)
{
	struct periodic_info info;
	struct timeval ts;
	FILE *fp;


//	printf("Thread 1 period 100ms\n");
	make_periodic(100000, &info);
	while (run_state) { // @@@ may change this to look for an exit cue, or do that in main

   GPIO_CLR = 1<<STROBE_PIN;  // make it low true
   GPIO_CLR = 1<<STROBE_PIN;
// write time of day to a file here, as close as we can to when we set the pin
// this seems to take around 1ms on a pi3b
	gettimeofday(&ts,NULL);
	fp = fopen("/tmp/time.txt","w");
	fprintf(fp,"%010ld.%06ld\n",ts.tv_sec,ts.tv_usec);
	fclose(fp);
	if (debug) printf("%010ld.%06ld\n",ts.tv_sec,ts.tv_usec);
	
   usleep (10); // let pin stay low awhile
   GPIO_SET = 1<<STROBE_PIN;  // set high again till hext hit
   GPIO_SET = 1<<STROBE_PIN;
		thread_1_count++;
		wait_period(&info);
	}
	return NULL;
}
////////////////////////////////////////////////////////////////////////////////////////////////////
void setup_io()
{
   /* open /dev/mem */
   if ((mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0) {
      printf("can't open /dev/mem \n");
      exit(-1);
   }

   /* mmap GPIO */
   gpio_map = mmap(
      NULL,             //Any adddress in our space will do
      BLOCK_SIZE,       //Map length
      PROT_READ|PROT_WRITE,// Enable reading & writting to mapped memory
      MAP_SHARED,       //Shared with other processes
      mem_fd,           //File to map
      GPIO_BASE         //Offset to GPIO peripheral
   );

   close(mem_fd); //No need to keep mem_fd open after mmap

   if (gpio_map == MAP_FAILED) {
      printf("mmap error %d\n", (int)gpio_map);//errno also set!
      exit(-1);
   }

// Always use volatile pointer!
  gpio = (volatile unsigned *)gpio_map;
 // Set GPIO pin STROBE_PIN to output, and high - we'll use falling edge in arduino to trigger
  INP_GPIO(STROBE_PIN); // must use INP_GPIO before we can use OUT_GPIO
  OUT_GPIO(STROBE_PIN); // above is original author comment, I don't have a clue (DC)
  GPIO_SET = 1<<STROBE_PIN;
  GPIO_SET = 1<<STROBE_PIN; // I tell you twice
} // setup_io
////////////////////////////////////////////////////////////////////////////////////////////////////
int main(int argc, char *argv[])
{
	pthread_t t_1;
	pthread_attr_t attr;
	int sockfd; 
	char buffer[MAXLINE]; 
	struct sockaddr_in servaddr , cliaddr; 

//	int i;
	unsigned char c;
	skeleton_daemon();


        run_state = 0;
    	setup_io();

	// Creating socket file descriptor 
	if ( (sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0 ) { 
		if (debug) perror("socket creation failed"); 
		exit(EXIT_FAILURE); 
	} 
	
	memset(&servaddr, 0, sizeof(servaddr)); // start clean
	memset(&cliaddr, 0, sizeof(cliaddr)); 

	
	// Filling server information 
	servaddr.sin_family = AF_INET; // IPv4 // @@@ maybe AF_UNIX instead?
	servaddr.sin_addr.s_addr = INADDR_ANY; 
	servaddr.sin_port = htons(PORT); 
	
	// Bind the socket with the server address 
	if ( bind(sockfd, (const struct sockaddr *)&servaddr, 
			sizeof(servaddr)) < 0 ) 
	{ 
		if (debug) perror("bind failed"); 
		exit(EXIT_FAILURE); 
	} 


	pthread_attr_init(&attr); // attributes for the thread
	pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED); // so will auto-return resources when it dies

	if (debug) printf("Periodic thread using timerfd\n");
	syslog (LOG_NOTICE, "strobe daemon started.");

	
	while (1)
	{

	int len, n; 
	n = recvfrom(sockfd, (char *)buffer, MAXLINE, 
				MSG_WAITALL, ( struct sockaddr *) &cliaddr, 
				&len); 
	buffer[n] = '\0'; 
	if (debug) printf("Client said : %s\n", buffer); 
	c = buffer[0]; // this is all one character messages (or should be)

	 switch (c)
	  {
		case 'g':
		case 'G':
		run_state = 1;
		pthread_create(&t_1, &attr, thread_1, NULL);
		if (debug) printf("go set\n");
		break;
		case 's':
		case 'S':
		run_state = 0;
		break;
		case 'q':
		case 'Q':
		run_state = 0;
		GPIO_SET = 1<<STROBE_PIN; 	//leave high

		if (debug) printf("terminating\n");
		syslog (LOG_NOTICE, "strobe daemon terminated.");
		closelog();
		return 0;
		break; //*/
	  } // switch
	} // while 1
	sleep(1); // handle possible stop race condition
	GPIO_SET = 1<<STROBE_PIN; 	//leave high
	if (debug) printf("thread1 %d iterations\n", thread_1_count);
	return 0;
}

