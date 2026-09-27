SRCS := $(wildcard *.c)
EXECS := $(SRCS:.c=)

all: $(EXECS)

%: %.c
	$(CC) -o $@ $<

clean:
	rm -f $(EXECS)
