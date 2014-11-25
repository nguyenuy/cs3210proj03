CFLAGS := -Wall $(shell pkg-config fuse libexif json --cflags) $(shell curl-config --cflags) $(shell Magick-config --cflags)
LDFLAGS := $(shell pkg-config fuse libexif json --libs) $(shell curl-config --libs) $(shell Magick-config --libs) $(shell MagickWand-config --libs)

targets = mypfs hello

all: $(targets)

hello: fuse_sel_example.c
	$(CC) -o $@ $< $(CFLAGS) $(LDFLAGS)

mypfs: mypfs.c
	$(CC) -o $@ $< $(CFLAGS) $(LDFLAGS)

ypfs: ypfs.c
	$(CC) -o $@ $< $(CFLAGS) $(LDFLAGS)

clean:
	rm -f *.o
	rm -f $(targets)
