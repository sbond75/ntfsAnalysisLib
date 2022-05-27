# https://stackoverflow.com/questions/287259/minimum-c-make-file-for-linux

SOURCES_CPP := main.cpp
SOURCES_C := tools.c
OBJS := $(SOURCES_CPP:.cpp=.o) $(SOURCES_C:.c=.o)
CPPFLAGS:=$(CPPFLAGS) -std=c++17 -O0 -g3 -ggdb3
LIBS=-lgmp

.PHONY: all
all: a.out

# Compile the binary 'a.out' by calling the compiler with cflags, ldflags, and any libs (if defined) and the list of objects.
a.out: $(OBJS)
	$(CXX) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LIBS)

# (Use the default Makefile rules ( https://www.gnu.org/software/make/manual/html_node/Catalogue-of-Rules.html ) instead of the below:)
# Get a .o from a .cpp by calling compiler with cflags and includes (if defined)
# .cpp.o:
# 	$(CC) $(CFLAGS) $(INCLUDES) -c $<
