CFLAGS := -Wall -Wextra -pedantic -std=gnu99 -g -O2
PROGRAMS := bin2c mkdmadata mkldscript

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
  DETECTED_OS=linux
else ifeq ($(UNAME_S),Darwin)
  DETECTED_OS=macos
else
  $(error Unsupported OS: $(UNAME_S))
endif

ifeq ($(shell command -v clang >/dev/null 2>&1; echo $$?),0)
  CC := clang
else
  CC := gcc
endif

LLD ?= 0

ifeq ($(shell command -v ld.lld >/dev/null 2>&1; echo $$?),0)
  LLD := 1
endif

ifneq ($(LLD),0)
  CFLAGS += -fuse-ld=lld
endif

all: $(PROGRAMS)
	$(MAKE) -C audio
	$(MAKE) -C assets

clean:
	$(RM) $(PROGRAMS) $(addsuffix .exe,$(PROGRAMS))
	$(MAKE) -C audio clean
	$(MAKE) -C assets clean

distclean: clean
	$(MAKE) -C audio distclean
	$(MAKE) -C assets distclean

.PHONY: all clean distclean

bin2c_SOURCES              := bin2c.c
mkdmadata_SOURCES          := mkdmadata.c spec.c util.c
mkldscript_SOURCES         := mkldscript.c spec.c util.c


define COMPILE =
$(1): $($1_SOURCES)
	$(CC) $(CFLAGS) $$^ -o $$@
endef

$(foreach p,$(PROGRAMS),$(eval $(call COMPILE,$(p))))

