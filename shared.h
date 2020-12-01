// Author: Adam Wilson
// Date: 12/2/2020

#include <stdlib.h>
#include <limits.h>
#include <stdio.h> 
#include <sys/sem.h>

#define BILLION 1000000000
#define MILLION 1000000

// process actions
enum action { readReq, writeReq, confirm, terminate, save };

// declare semaphore union
union semun {
    int val;
    struct semid_ds* buf;
    unsigned short* array;
};

// time struct
struct mtime {
	int sec;
	long int ns;
};

// holds cumulative statistics
struct statistics {
	struct mtime processAccessTimes[18];
	int processRerences[18];
	int numReferences;
	int numPageFaults;
};

// holds message contents/info
struct msgbuf {
    long type;
	struct msgInfo {
		int pid;
		int address;
		enum action act;
	} info;
};

// page
struct page {
	int frameNum;
	int validBit;
};

// frame
struct frame {
	int pid;
	int pageNum;
	int occupied;
	int refByte;
	int dirtyBit;
};

// shared memory segment
struct shmseg {
	struct mtime currentTime;
	struct statistics stats;
	struct page pageTables[18][32];
	struct frame frameTable[256];
	int PIDmap[18];
};

// page request
struct pageRequest {
	int pid;
	int address;
	enum action act;
};

// queue struct to hold pageRequest structs
struct pageQueue {
    int front, rear, size;
    int capacity;
    struct pageRequest* array;
};

void mWait(int*);
struct mtime addTime(struct mtime, int, long);
struct mtime subtractTime(struct mtime, struct mtime);
int compareTimes(struct mtime, struct mtime);
double timeToDouble(struct mtime);
struct pageQueue* createQueue();
int isFull(struct pageQueue*);
int isEmpty(struct pageQueue*);
void enqueue(struct pageQueue*, struct pageRequest);
struct pageRequest dequeue(struct pageQueue*);
int front(struct pageQueue*);
int rear(struct pageQueue*);