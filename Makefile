# Makefile adaptable para selector_playgsf

# Variables por defecto (pueden venir del entorno)
CXX      ?= g++
CC       ?= gcc
CROSS_COMPILE ?=
SYSROOT  ?=

# Fuentes
SRC := selector_playgsf.cpp VBA/psftag.c
OBJ := $(SRC:.cpp=.o)
OBJ := $(OBJ:.c=.o)

TARGET := selector_playgsf

# Configuración según entorno
ifeq ($(strip $(CROSS_COMPILE)),)
    # Compilación local
    CFLAGS   := -std=c++14 -Wall -O2 -DLINUX $(shell sdl2-config --cflags)
    LFLAGS   := $(shell sdl2-config --libs) -lSDL2_ttf
    INCLUDES :=
else
    # Compilación cruzada (solo si SYSROOT está definido)
    ifneq ($(strip $(SYSROOT)),)
        CXX      := $(CROSS_COMPILE)g++
        CC       := $(CROSS_COMPILE)gcc
        CFLAGS   := -std=c++14 -Wall -O2 -DLINUX --sysroot=$(SYSROOT) -I$(SYSROOT)/usr/include/SDL2
        LFLAGS   := -L$(SYSROOT)/usr/lib -lSDL2 -lSDL2_ttf
    else
        # Si no hay SYSROOT, compila como local
        CFLAGS   := -std=c++14 -Wall -O2 -DLINUX $(shell sdl2-config --cflags)
        LFLAGS   := $(shell sdl2-config --libs) -lSDL2_ttf
        INCLUDES :=
    endif
endif

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CFLAGS) -o $@ $^ $(LFLAGS)

%.o: %.cpp
	$(CXX) $(CFLAGS) $(INCLUDES) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
