# way one
FFMPEG_LIBS=    libavdevice                        \
				libavformat                        \
				libavfilter                        \
				libavcodec                         \
				libswresample                      \
				libswscale                         \
				libavutil                          \


CFLAGS += -Wall -g
CFLAGS := $(shell pkg-config --cflags $(FFMPEG_LIBS)) $(CFLAGS)
LDLIBS := $(shell pkg-config --libs $(FFMPEG_LIBS)) $(LDLIBS)

EXAMPLES= demo



OBJS=$(addsuffix .o,$(EXAMPLES))

all: $(OBJS) $(EXAMPLES)


muxing:            LDLIBS += -lm

.phony: clean
clean:
	$(RM) $(EXAMPLES) $(OBJS)

# way two
# TARGET=streamer
# SRC=$(wildcard *.c)
# OBJ=$(patsubst %.c, %.o, $(SRC))


# all: $(TARGET)

# $(TARGET):$(OBJ)
#	 $(CC) $(OBJ) -o $(TARGET) \
#		 -lavformat -lavcodec -lavutil -lswresample -lz -lm -lpthread -ldl -Wl,-Bdynamic -g
# %.o:%.c
#	 echo $@ ---- $<
#	 $(CC) -c $< -o $@ -Wno-deprecated-declarations -g
# .PHONY:clean
# clean:
#	$(RM) $(TARGET) $(OBJ)


