CC = gcc
CFLAGS = -g -o
TARGET_1 = oss
TARGET_2 = user_proc
OBJS_1 = oss.c shared.c
OBJS_2 = user_proc.c shared.c

all: $(TARGET_1) $(TARGET_2)

$(TARGET_1): $(OBJS_1)
	$(CC) $(CFLAGS) $(TARGET_1) $(OBJS_1)
$(TARGET_2): $(OBJS_2)
	$(CC) $(CFLAGS) $(TARGET_2) $(OBJS_2)
clean:
	/bin/rm -f *.o *.log $(TARGET_1) $(TARGET_2)
