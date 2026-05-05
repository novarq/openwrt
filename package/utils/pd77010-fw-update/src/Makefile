CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2

TARGET = pd77010-fw-update

$(TARGET): pd77010-fw-update.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

.PHONY: clean
