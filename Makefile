VERSION=0.3.0
BUILD := $(VERSION)-$(shell git rev-parse --short HEAD)$(shell git diff --quiet || echo "-dirty")

RGB_LIB_DISTRIBUTION=external/rpi-rgb-led-matrix
RGB_INCDIR=$(RGB_LIB_DISTRIBUTION)/include
RGB_LIBDIR=$(RGB_LIB_DISTRIBUTION)/lib
RGB_LIBRARY_NAME=rgbmatrix
RGB_LIBRARY=$(RGB_LIBDIR)/lib$(RGB_LIBRARY_NAME).a

HTTP_LIB_DIR=external/cpp-httplib
JSON_LIB_DIR=external/json/single_include

LDFLAGS+=-L $(RGB_LIBDIR) -l$(RGB_LIBRARY_NAME) -lrt -lm -lpthread -lssl -lcrypto

CXX=g++
CXXFLAGS=-std=c++23 -Wall -Wextra -DBUILD=\"$(BUILD)\"

$(RGB_LIBRARY):
	$(MAKE) -C $(RGB_LIBDIR)

build_folder:
	rm -rf build
	mkdir build

dev: $(RGB_LIBRARY) build_folder
	$(CXX) $(CXXFLAGS) src/main.cpp -O0 -DCPPHTTPLIB_OPENSSL_SUPPORT -o build/ptrans-rgb -isystem $(HTTP_LIB_DIR) -isystem $(JSON_LIB_DIR) -isystem $(RGB_INCDIR) $(LDFLAGS)

prod: $(RGB_LIBRARY) build_folder
	$(CXX) $(CXXFLAGS) src/main.cpp -O3 -DCPPHTTPLIB_OPENSSL_SUPPORT -o build/ptrans-rgb -isystem $(HTTP_LIB_DIR) -isystem $(JSON_LIB_DIR) -isystem $(RGB_INCDIR) $(LDFLAGS)
