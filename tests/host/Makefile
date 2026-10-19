# Host unit tests for the pure-C protocol and census code. No Flipper SDK.
#   make -C tests/host
CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Wno-unused-parameter -g -O0 -fsanitize=address,undefined
SRC = ../../govee_protocol.c ../../govee_census.c
BUILD = build

all: test

$(BUILD)/test_census: test_census.c $(SRC) ../../govee_protocol.h ../../govee_census.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_census.c $(SRC)

test: $(BUILD)/test_census
	./$(BUILD)/test_census

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
