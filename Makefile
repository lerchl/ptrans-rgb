VERSION=0.2.0
BUILD := $(shell git describe --tags --always --dirty | sed 's/^v//')

RGB_LIB_DISTRIBUTION=external/rpi-rgb-led-matrix
RGB_INCDIR=$(RGB_LIB_DISTRIBUTION)/include
RGB_LIBDIR=$(RGB_LIB_DISTRIBUTION)/lib
RGB_LIBRARY_NAME=rgbmatrix
RGB_LIBRARY=$(RGB_LIBDIR)/lib$(RGB_LIBRARY_NAME).a

HTTP_LIB_DIR=external/cpp-httplib
JSON_LIB_DIR=external/json/single_include

LDFLAGS+=-L $(RGB_LIBDIR) -l$(RGB_LIBRARY_NAME) -lrt -lm -lpthread -lssl -lcrypto

CXX=g++
CXXFLAGS=-std=c++23 -Wall -Wextra 

$(RGB_LIBRARY):
	$(MAKE) -C $(RGB_LIBDIR)

build_folder:
	rm -rf build
	mkdir build

dev: $(RGB_LIBRARY) build_folder
	$(CXX) $(CXXFLAGS) src/main.cpp -DAPP_VERSION=\"$(BUILD)\" -O0 -o build/ptrans-rgb -isystem $(HTTP_LIB_DIR) -isystem $(JSON_LIB_DIR) -isystem $(RGB_INCDIR) $(LDFLAGS)

prod: $(RGB_LIBRARY) build_folder 
	$(CXX) $(CXXFLAGS) src/main.cpp -DAPP_VERSION=\"$(VERSION)\" -O3 -DCPPHTTPLIB_OPENSSL_SUPPORT -o build/ptrans-rgb -isystem $(HTTP_LIB_DIR) -isystem $(JSON_LIB_DIR) -isystem $(RGB_INCDIR) $(LDFLAGS)
