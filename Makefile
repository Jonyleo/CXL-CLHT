#################################
# Architecture dependent settings
#################################

CFLAGS = -D_GNU_SOURCE -pthread

DEBUG_FLAGS=-Wall -ggdb -g -DDEBUG
CFLAGS += -O0 -fno-inline 


TOP := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

LIBS+=-L$(TOP)/external/lib -L$(TOP) -L$(TOP)/external/shm_alloc_devdax/src

SRCPATH := $(TOP)/src
MAININCLUDE := $(TOP)/include

# default setings
PLATFORM=-DDEFAULT
GCC=gcc
BMARK_GCC=$(GCC)
PLATFORM_NUMA=0
OPTIMIZE=
LIBS += -lrt -lpthread -lm  -lclht -lssmem -lshm_alloc  -lsigsegv

UNAME := $(shell uname -n)

CFLAGS += $(PLATFORM)
CFLAGS += $(OPTIMIZE)
CFLAGS += $(DEBUG_FLAGS)

INCLUDES := -I$(MAININCLUDE) -I$(TOP)/external/include -I$(TOP)/external/shm_alloc_devdax/src
OBJ_FILES := clht_gc.o clht_shm.o $(TOP)/external/shm_alloc_devdax/src/libshm_alloc.so

SRC := src

BMARKS := bmarks

#MAIN_BMARK := $(BMARKS)/test.c     # no memory allocation
#MAIN_BMARK := $(BMARKS)/test_ro.c  # read-only benchmark
#MAIN_BMARK := $(BMARKS)/test_mem.c  # memory allocation
MAIN_BMARK := $(BMARKS)/randuration.c  
BMARK_GCC=g++

ALL = 	clht_lf_res

default: all

dependencies:
	./scripts/make_dependencies.sh

all: $(ALL)

.PHONY: $(ALL) \
	libclht_lf_res.a


%.o:: $(SRC)/%.c 
	$(GCC) $(CFLAGS) $(INCLUDES) -o $@ -c $<

clht_gc_linked.o: $(SRC)/clht_gc.c
	$(GCC) -DCLHT_LINKED $(CFLAGS) $(INCLUDES) -o clht_gc_linked.o -c $(SRC)/clht_gc.c

################################################################################
# library
################################################################################

TYPE = clht_lf_res
OBJ = $(TYPE).o
lib$(TYPE).a: $(OBJ_FILES) $(OBJ) 
	@echo Archive name = libclht.a
	ar -d libclht.a *
	ar -r libclht.a clht_lf_res.o $(OBJ_FILES)


TYPE = clht_lf_res
$(TYPE): $(MAIN_BMARK) lib$(TYPE).a 
	$(GCC) -DLOCKFREE_RES $(CFLAGS) $(INCLUDES) $(MAIN_BMARK) -o clht_lf_res $(LIBS)

clean:				
	rm -f *.o *.a clht_*
	make -C $(TOP)/external/shm_alloc_devdax/src/ clean

$(TOP)/external/shm_alloc_devdax/src/libshm_alloc.so: $(TOP)/external/shm_alloc_devdax/src/*
	make -C $(TOP)/external/shm_alloc_devdax/src/