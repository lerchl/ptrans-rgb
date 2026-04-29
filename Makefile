VERSION=1.1.0
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
CXXFLAGS=-std=c++23 -Wall -Wextra

SRCS := $(shell find src -name '*.cpp')
OBJS := $(patsubst src/%.cpp, build/obj/%.o, $(SRCS))

$(RGB_LIBRARY):
	$(MAKE) -C $(RGB_LIBDIR)

build/obj/%.o: src/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(DEFINES) $(INCLUDES) -c $< -o $@

build_folder:
	rm -rf build
	mkdir -p build/obj

dev: CXXFLAGS += -O0
dev: DEFINES := -DAPP_VERSION=\"$(BUILD)\"
dev: $(RGB_LIBRARY) build_folder $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) $(DEFINES) -o build/ptrans-rgb $(LDFLAGS)

prod: CXXFLAGS += -O3
prod: DEFINES := -DAPP_VERSION=\"$(VERSION)\"
prod: $(RGB_LIBRARY) build_folder $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) $(DEFINES) -o build/ptrans-rgb $(LDFLAGS)

INCLUDES := -DCPPHTTPLIB_OPENSSL_SUPPORT \
            -isystem $(HTTP_LIB_DIR) \
            -isystem $(JSON_LIB_DIR) \
            -isystem $(RGB_INCDIR)
