CFLAGS := -Wall $(shell pkg-config fuse libexif json --cflags) 
LDFLAGS := $(shell pkg-config fuse libexif --libs)

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
