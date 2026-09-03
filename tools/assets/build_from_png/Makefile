CFLAGS := -Wall -Werror -O3

ifeq ($(shell $(CC) --version | grep clang),)
  OMPFLAGS := -fopenmp
else
  OMPFLAGS :=
  # clang: VLA-with-constant-bound is a GNU extension warning; harmless
  CFLAGS += -Wno-gnu-folding-constant
endif

default: build_from_png

clean:
	$(RM) build_from_png

.PHONY: default clean

build_from_png: build_from_png.c ../n64texconv/libn64texconv.a
	$(CC) $(CFLAGS) -o $@ $^ $(OMPFLAGS) -lz -lm
