# $Id: Makefile,v 1.15 2017/05/29 10:29:35 karn Exp karn $
INCLUDES=-I /opt/local/include
COPTS=-g -std=gnu11 -pthread -Wall -funsafe-math-optimizations 
CFLAGS=$(COPTS) $(INCLUDES)

all: radio control funcube

clean:
	rm -f *.o radio control funcube libfcd.a

funcube: funcube.o gr.o libfcd.a
	$(CC) -g -o $@ $^ -lasound -lusb-1.0 -lpthread -lm

control: control.o modes.o
	$(CC) -g -o $@ $^ -lm

radio: main.o radio.o demod.o am.o fm.o ssb.o iq.o cam.o filter.o display.o modes.o audio.o misc.o
	$(CC) -g -o $@ $^ -lasound  -lfftw3f_threads -lfftw3f -lpthread -lncurses -lm

libfcd.a: fcd.o hid-libusb.o
	ar rv $@ $^
	ranlib $@

am.o: am.c dsp.h filter.h radio.h audio.h
audio.o: audio.c dsp.h audio.h
cam.o: cam.c dsp.h filter.h radio.h audio.h
control.o: control.c command.h
demod.o: demod.c radio.h
display.o: display.c radio.h audio.h dsp.h
fcd.o: fcd.c fcd.h hidapi.h fcdhidcmd.h
filter.o: filter.c dsp.h filter.h
fm.o: fm.c dsp.h filter.h radio.h audio.h
funcube.o: funcube.c fcd.h fcdhidcmd.h hidapi.h sdr.h command.h dsp.h rtp.h
gr.o: gr.c sdr.h
hid-libusb.o: hid-libusb.c hidapi.h
main.o: main.c radio.h filter.h dsp.h audio.h command.h rtp.h
misc.o: misc.c
modes.o: modes.c command.h
radio.o: radio.c command.h radio.h filter.h dsp.h audio.h
ssb.o: ssb.c dsp.h filter.h radio.h audio.h
iq.o: iq.c dsp.h filter.h radio.h audio.h

