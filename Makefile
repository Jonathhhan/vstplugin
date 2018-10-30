# Makefile to build library 'vsthost~' for Pure Data.
# Needs Makefile.pdlibbuilder as helper makefile for platform-dependent build
# settings and rules.
#
# use : make PDDIR=/path/to/pure-data
#
# The following command will build the external and install the distributable 
# files into a subdirectory called build :
#
# make install PDDIR=/path/to/pure-data PDLIBDIR=./build

lib.name = vsthost~

class.sources = src/vsthost~.cpp

common.sources = src/VSTPlugin.cpp src/VST2Plugin.cpp

# all extra files to be included in binary distribution of the library
datafiles = 

cflags = -Wno-unused -Wno-unused-parameter -std=c++11 -g

include pd-lib-builder/Makefile.pdlibbuilder
