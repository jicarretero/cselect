# LIBS=-lX11 -lcjson
LIBS=

all: 
	gcc $(DEBUG) cselect.c -o cselect $(LIBS)

clean:
	rm -rf cselect 

debug: DEBUG = -g

debug: all

install: all
	cp -f cselect /usr/local/bin
