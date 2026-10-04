# Host builds of the portable code and its tests:
#   host_tests     the DSP (portable FFT backend)
#   meeting_tests  the meeting store's parser and accept policy (with ESP-IDF's cJSON.c)
#   ui_text_tests  the Meeting tab's word wrap and paging
#   make -C code/test        build and run them all
CXX      ?= c++
CC       ?= cc
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CFLAGS   ?= -O2
HDRS      = $(wildcard ../*.h) check.h
# cJSON: the same source the device links (ESP-IDF's json component).
CJSON_DIR ?= $(HOME)/.platformio/packages/framework-espidf/components/json/cJSON

SRCS         = host_tests.cpp ../dsp.cpp ../doa.cpp ../beamformer.cpp ../processor.cpp ../sync_check.cpp
MEETING_SRCS = meeting_tests.cpp ../meeting_parse.cpp ../meeting_demo.cpp
UI_TEXT_SRCS = ui_text_tests.cpp ../ui_text.cpp

run: host_tests meeting_tests ui_text_tests
	./host_tests
	./meeting_tests
	./ui_text_tests

host_tests: $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -I.. -o $@ $(SRCS)

cJSON.o: $(CJSON_DIR)/cJSON.c
	$(CC) $(CFLAGS) -c -o $@ $<

meeting_tests: $(MEETING_SRCS) $(HDRS) cJSON.o
	$(CXX) $(CXXFLAGS) -I.. -I$(CJSON_DIR) -o $@ $(MEETING_SRCS) cJSON.o

ui_text_tests: $(UI_TEXT_SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -I.. -o $@ $(UI_TEXT_SRCS)

clean:
	rm -f host_tests meeting_tests ui_text_tests cJSON.o

.PHONY: run clean
