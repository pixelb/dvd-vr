NAME := dvd-vr
VERSION := 0.8
PREFIX := /usr/local
DESTDIR :=

CC := gcc

CFLAGS+=-std=gnu99 -Wall -Wextra -Wpadded -DVERSION='"$(VERSION)"'

# Use `make DEBUG=1` to build debugging version
ifeq ($(DEBUG),1)
    CFLAGS+=-ggdb
else
    CFLAGS+=-O3 -DNDEBUG
endif

# Use iconv when available
HAVE_ICONV := $(shell echo "\#include <iconv.h>" | cpp >/dev/null 2>&1 && echo 1 || echo 0)
ifeq ($(HAVE_ICONV),1)
    CFLAGS+=-DHAVE_ICONV

    # Work around const warnings
    ICONV_CONST := $(shell (echo "\#include <iconv.h>"; echo "size_t iconv(iconv_t,char **,size_t*,char **,size_t*);") | gcc -xc -c - -o /dev/null 2>/dev/null || echo const)
    CFLAGS+=-DICONV_CONST="$(ICONV_CONST)"

    # Add -liconv where available/required like Mac OS X & CYGWIN for example
    NEED_LICONV := $(shell echo "int main(void){}" | $(CC) -xc -liconv - -o /dev/null 2>/dev/null && echo 1 || echo 0)
    ifeq ($(NEED_LICONV),1)
	LDFLAGS+=-liconv
    endif
else
    $(warning "Warning: title translation support disabled as libiconv not installed")
endif

# Strip debugging symbols if not debugging
ifneq ($(DEBUG),1)
    LDFLAGS+=-Wl,-S
endif

HOST := $(shell uname | tr '[:lower:]' '[:upper:]')
ifneq (,$(findstring CYGWIN,$(HOST)))
    EXEEXT := .exe
endif

BINARY := $(NAME)$(EXEEXT)
SOURCES := *.c
OBJECTS := $(patsubst %.c,%.o,$(wildcard $(SOURCES)))

#first target is the default
.PHONY: all
all: $(BINARY)

$(BINARY): $(OBJECTS)
	gcc $(LIBS) $(OBJECTS) $(LDFLAGS) -o $@

#if implicit rule for .c doesn't suffice, apply here
#%.o: %.c
#	gcc $(CFLAGS) $< -c

.PHONY: dist
dist: clean
	mkdir $(NAME)-$(VERSION)
	tar --exclude $(NAME)-$(VERSION) --exclude .svn --exclude .git -c . | (cd $(NAME)-$(VERSION) && tar -xp)
	tar c $(NAME)-$(VERSION) | gzip -9 > $(NAME)-$(VERSION).tar.gz
	-@rm -Rf $(NAME)-$(VERSION)

.PHONY: clean
clean:
	-@rm -f *.o $(BINARY) core*
	-@rm -Rf $(NAME)-$(VERSION)*

man/$(NAME).1: $(BINARY) man/$(NAME).x
	help2man --no-info --include=man/$(NAME).x ./$(BINARY) > man/$(NAME).1

.PHONY: man
man: man/$(NAME).1

datadir := $(PREFIX)/share
mandir := $(datadir)/man
man1dir = $(mandir)/man1
bindir = $(PREFIX)/bin

.PHONY: install
install: all
	-@mkdir -p $(DESTDIR)$(bindir)
	cp -a $(BINARY) $(DESTDIR)$(bindir)
	-@mkdir -p $(DESTDIR)$(man1dir)
	gzip -c man/$(NAME).1 > $(DESTDIR)$(man1dir)/$(NAME).1.gz

.PHONY:	uninstall
uninstall:
	rm $(DESTDIR)$(bindir)/$(BINARY)
	rm $(DESTDIR)$(man1dir)/$(NAME).1*
