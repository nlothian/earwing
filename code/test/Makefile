# Host build of the portable DSP (portable FFT backend) and its tests.
#   make -C code/test        build and run
CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
SRCS      = host_tests.cpp ../dsp.cpp ../doa.cpp ../beamformer.cpp ../processor.cpp ../sync_check.cpp
HDRS      = $(wildcard ../*.h)

run: host_tests
	./host_tests

host_tests: $(SRCS) $(HDRS)
	$(CXX) $(CXXFLAGS) -I.. -o $@ $(SRCS)

clean:
	rm -f host_tests

.PHONY: run clean
