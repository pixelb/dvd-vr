NAME := dvd-vr
VERSION := 0.7
TARFILE := $(NAME)-$(VERSION).tar.gz
HOST := $(shell uname | tr '[:lower:]' '[:upper:]')
CC := gcc

# Use `make DEBUG=1` to build debugging version
ifeq ($(DEBUG),1)
    CFLAGS+=-std=gnu99 -Wall -Wextra -Wpadded -ggdb
else
    CFLAGS+=-std=gnu99 -Wall -Wextra -Wpadded -O3 -DNDEBUG
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

ifneq (,$(findstring CYGWIN,$(HOST)))
    EXEEXT := .exe
endif

BINARY := $(NAME)$(EXEEXT)
SOURCES := *.c
OBJECTS := $(patsubst %.c,%.o,$(wildcard $(SOURCES)))

$(BINARY): ${OBJECTS}
	gcc ${LIBS} ${OBJECTS} ${LDFLAGS} -o $@

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
	-@rm -f *.o $(BINARY) core*
	-@rm -Rf $(TARFILE) $(NAME)-$(VERSION)
