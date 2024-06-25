# Variables
CXX = g++
LLVM_CONFIG = /usr/lib/llvm-17/bin/llvm-config
CXXFLAGS = -g -std=c++17 `$(LLVM_CONFIG) --cxxflags`
LDFLAGS = `$(LLVM_CONFIG) --ldflags`  -Wl,-rpath,`$(LLVM_CONFIG) --libdir`
LDLIBS = `$(LLVM_CONFIG) --libs`

# Targets
all: main

main: main.o DebugIR.o
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

main.o: main.cpp jit.hpp
	$(CXX) $(CXXFLAGS) -c $<

DebugIR.o: DebugIR.cpp DebugIR.hpp
	$(CXX) $(CXXFLAGS) -c $<

clean:
	rm -f *.o main