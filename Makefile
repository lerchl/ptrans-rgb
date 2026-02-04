RGB_LIB_DISTRIBUTION=external/rpi-rgb-led-matrix
RGB_INCDIR=$(RGB_LIB_DISTRIBUTION)/include
RGB_LIBDIR=$(RGB_LIB_DISTRIBUTION)/lib
RGB_LIBRARY_NAME=rgbmatrix
RGB_LIBRARY=$(RGB_LIBDIR)/lib$(RGB_LIBRARY_NAME).a

HTTP_LIB_DIR=external/cpp-httplib
JSON_LIB_DIR=external/json/single_include

LDFLAGS+=-L $(RGB_LIBDIR) -l$(RGB_LIBRARY_NAME) -lrt -lm -lpthread -lssl -lcrypto

CXX=g++
CXXFLAGS=-std=c++23 -Wall -Wextra -O3

$(RGB_LIBRARY):
	$(MAKE) -C $(RGB_LIBDIR)

build_folder:
	rm -rf build
	mkdir build

main: $(RGB_LIBRARY) build_folder
	$(CXX) $(CXXFLAGS) src/main.cpp -o build/ptrans-rgb -I $(HTTP_LIB_DIR) -I $(JSON_LIB_DIR) -I $(RGB_INCDIR) $(LDFLAGS)
