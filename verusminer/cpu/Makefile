# verusminer/cpu — Apple Silicon build, canonical VerusHash 2.2 + PBaaS
#
# Hash code (`canonical/`) is the unmodified hellcatz/verushash-node fork
# that LuckPool actually runs server-side — guarantees our miner-side hash
# matches the pool's hash by definition.

CXX      = clang++
CC       = clang
CFLAGS   = -O3 -march=armv8-a+crypto+sha2+aes -Wall -Wno-deprecated-declarations -I/opt/homebrew/include
CXXFLAGS = $(CFLAGS) -std=c++17 -Wno-unused-variable -Wno-shift-op-parentheses -Wno-#warnings -DHAVE_CONFIG_H=0
LDFLAGS  = -L/opt/homebrew/lib -lsodium

# Local objects (stratum + haraka primitives we share with canonical)
LOCAL_OBJS = haraka_portable.o haraka.o stratum.o

# Canonical VerusCoin/verushash-node objects (built from canonical/)
CANON_OBJS = canonical/verus_hash.o canonical/verus_clhash.o canonical/verus_clhash_portable.o

# Metal GPU bridge — Objective-C++ wrapper that lets main.cpp dispatch
# Metal kernels. Links Metal + Foundation frameworks.
METAL_OBJ = ../metal/metal_bridge.o

all: verusminer

%.o: %.c
	$(CC) $(CFLAGS) -I. -Icanonical -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -I. -Icanonical -c $< -o $@

canonical/%.o: canonical/%.cpp
	$(CXX) $(CXXFLAGS) -I. -Icanonical -c $< -o $@

$(METAL_OBJ): ../metal/metal_bridge.mm
	$(CXX) -O3 -std=c++17 -ObjC++ -fobjc-arc -c $< -o $@

verusminer: main.o $(LOCAL_OBJS) $(CANON_OBJS) $(METAL_OBJ)
	$(CXX) $(CXXFLAGS) $^ -o $@ -lpthread $(LDFLAGS) \
	  -framework Metal -framework Foundation

bench: verusminer
	./verusminer

quick: verusminer
	./verusminer quick

clean:
	rm -f *.o canonical/*.o ../metal/metal_bridge.o verusminer verusminer_portable
