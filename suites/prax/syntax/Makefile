# Builds a C program and its tests, with dependency files and a debug variant.
CC ?= cc
CFLAGS := -std=c17 -Wall -Wextra -O2
LDLIBS := -lm
BUILD := build
SOURCES := $(wildcard src/*.c)
OBJECTS := $(patsubst src/%.c,$(BUILD)/%.o,$(SOURCES))

ifeq ($(DEBUG),1)
    CFLAGS += -g -O0 -DDEBUG
endif

.PHONY: all test clean

all: $(BUILD)/ring

$(BUILD)/ring: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD):
	mkdir -p $@

test: $(BUILD)/ring
	@echo "running tests"
	./$(BUILD)/ring --self-test

clean:
	rm -rf $(BUILD)

-include $(OBJECTS:.o=.d)
