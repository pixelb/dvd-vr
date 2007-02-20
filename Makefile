NAME=dvd-vr
VERSION=0.1
TARFILE=$(NAME)-$(VERSION).tar.gz

CC=gcc
CFLAGS=-DNDEBUG -std=c99 -Wall -Wpadded
#CFLAGS=-ggdb -std=c99 -Wall -Wpadded

BINARY := $(NAME)
SOURCES := *.c
OBJECTS := $(patsubst %.c,%.o,$(wildcard $(SOURCES)))

$(BINARY): ${OBJECTS}
	gcc ${LIBS} ${OBJECTS} ${LDFLAGS} -o $@
	strip $@

all: $(BINARY)

#if implicit rule for .c doesn't suffice, apply here
#%.o: %.c
#	gcc $(CFLAGS) $< -c

dist: clean
	mkdir $(NAME)-$(VERSION)
	tar --exclude $(NAME)-$(VERSION) --exclude .svn -c . | (cd $(NAME)-$(VERSION) && tar -xp)
	tar c $(NAME)-$(VERSION) | gzip -9 > $(TARFILE)
	-@rm -Rf $(NAME)-$(VERSION)

clean:
	-@rm -f *.o $(BINARY) core.*
	-@rm -Rf $(TARFILE) $(NAME)-$(VERSION)
