# Timing_generator
Hard timing generator for Raspberry pi

This code will generate some fairly accurate timing that toggles an IO pin on a raspberry pi.  I'm using it to 
synchronize a bunch of arduino slaves used in a data aquisition setup, to ensure they all are triggered at the same times.
While there is some jitter - it's the glory of any multitasking operating systemt to pre-empt, in practice here 
it seems timing jitter is under 1 millisecond for a 100ms strobe interval.  Since I'm not doing nyquisty kinds of things
in my data acq, that's acceptable, and I was frankly a bit suprised it could be this good in linux.

The code supports either raspberry pi 3 or 4, via a define you change up top - read the comments, the base address for the
IO is different and you have to change which one you use, depending.  I'm a little hesitant to spec line numbers as
keeping all the doc in sync can be a problem.  But right now, that define is on line 75.

I also provide a .service file that can be used to autostart the strobe daemon.

To use, copy the strobe executable to /usr/bin (assuming it's the right build for your pi).
Since you need to be root to do that, the owner will now be root - make sure that's the case as this
needs root privelge to map the IO memory.

As it is up here, it's built for a pi 4.  If you have a pi 3, edit the code and use the makefile to rebuild it.
Then copy strobe.service to /etc/systemd/system (or whatever place you or poettering thinks is better).
If you copy it to other than the above directory, you'll have to use systemctl to enable it.
This will then run from boot, but not do anything till you send it a message on UDP socket 42742.

The commands are:
* g or G to start the 100 ms pulse train (low true).
* s or S to stop it.
* q or Q to terminate the daemon (used for initial testing).

You can change the code to use a different pin, it's a define up top of the c code. (line 93)
Currently the pulse is low true, and of somewhat variable length. On my pi, it seems any preemption happens during
the low true pulse.  The way I'm using it this doesn't matter - slaves are triggered by the falling edge.
In this release, a file is opened and the current time is written to the first line.  (lines 227-231)
Since those are system calls, that's probably why I notice some time variation there - it's yeilding to the system.
I'm using a file in /tmp, which on the pi is in ram so it's fairly quick.  If you don't need this, comment it out.

You can also change the period - I needed 100ms (100 thousand microseconds) so that's what is in the code.  (line 220)

If you're just playing around, you might want to comment out the call that daemonizes this, (line 286) and turn on debug.

Here, I'm using a perl gui to send the messages to start and stop the strobe signal, and it's tied to interrupt inputs
on the arduino slaves to trigger them to collect some a/d and counter samples, and report that back over USB serial.
Since I used AF_INET for the socket def - you could send this messages from elsewhere if you wish, a bug or a feature.
I use "packet sender" extensivly here to test things like this, it's very handy.  https://packetsender.com/

Mostly, this was cobbling together some other examples from online, and adapting to my needs.  It's a decent example of
how to do these things.
