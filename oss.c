// Author: Adam Wilson
// Date: 12/2/2020

#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>
#include <sys/time.h>
#include <sys/ipc.h> 
#include <sys/shm.h> 
#include <sys/types.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include "shared.h"

// intra-file globals
FILE* fp;
int msqid, shmid, semid, currentChildren, totalProcs, lastPID, xVal;
int frameMap[256];
union semun sem;
struct sembuf p = { 0, -1, SEM_UNDO };
struct sembuf v = { 0, +1, SEM_UNDO };
struct shmseg* shmptr;
struct pageQueue* pQueue;
int numTermed = 0;

// creates a shared memory segment, a message queue, and a semaphore
void createMemory() {
	key_t shmkey, msqkey, semkey;
	
	shmkey = ftok("oss", 137);
	shmid = shmget(shmkey, sizeof(struct shmseg), 0666 | IPC_CREAT);
	if (shmid == -1) {
		perror("oss: Error");
		exit(-1);
	}

	shmptr = shmat(shmid, (void*)0, 0);
	if (shmptr == (void*)-1) { perror("oss: Error"); }

    semkey = ftok("oss", 484);
    semid = semget(semkey, 1, 0666 | IPC_CREAT);
    if (semid < 0) { perror("semget"); }
	sem.val = 1;
    if (semctl(semid, 0, SETVAL, sem) < 0) { perror("semctl"); }

	msqkey = ftok("oss", 731);
	msqid = msgget(msqkey, 0666 | IPC_CREAT);
	if (msqid == -1) { perror("oss: Error"); }
}

// outputs stats, waits for children, destroys message queue and semaphore, and detaches and destroys shared memory
void terminateOSS() {
	int i, j, status;
	printf( "\n\nOSS ran for %.4f s\n", timeToDouble(shmptr->currentTime));
	// fprintf(fp, "Average lifetime: %.3f s / process\n", timeToDouble(shmptr->stats.lifeTime) / shmptr->stats.numComplete);
	// fprintf(fp, "Average sleep time: %.3f s / process\n", timeToDouble(shmptr->stats.waitTime) / shmptr->stats.numComplete);
	printf("Memory references: %.3f / s\n", shmptr->stats.numReferences / timeToDouble(shmptr->currentTime));
	printf("Page faults: %.3f / memory access\n", shmptr->stats.numPageFaults / (double)shmptr->stats.numReferences);
	printf("num complete: %d\n", numTermed);
	fclose(fp);
	for (i = 0; i < currentChildren; i++) { mWait(&status); }
	if (msgctl(msqid, IPC_RMID, NULL) == -1) { perror("oss: msgctl"); }
    if (semctl(semid, 0, IPC_RMID, 0) == -1) { perror("Can't RPC_RMID"); }
	if (shmdt(shmptr) == -1) { perror("oss: Error"); }
	if (shmctl(shmid, IPC_RMID, 0) == -1) {
		perror("oss: Error");
		exit(-1);
	}
	exit(0);
}

// deletes output.log if it exists, then creates it in append mode
void setupFile() {
	fp = fopen("output.log", "w+");
	if (fp == NULL) { perror("oss: Error"); }
	fclose(fp);
	fp = fopen("output.log", "a");
	if (fp == NULL) { perror("oss: Error"); }
}

// sends message to stderr, then kills all processes in this process group, which is ignored by parent
static void interruptHandler(int s) {
	fprintf(stderr, "\nInterrupt recieved\n");
	signal(SIGQUIT, SIG_IGN);
	kill(-getpid(), SIGQUIT);
	terminateOSS();
}

// sets up sigaction for SIGALRM
static int setupAlarmInterrupt(void) {
	struct sigaction sigAlrmAct;
	sigAlrmAct.sa_handler = interruptHandler;
	sigAlrmAct.sa_flags = 0;
	sigemptyset(&sigAlrmAct.sa_mask);
	return (sigaction(SIGALRM, &sigAlrmAct, NULL));
}

// sets up sigaction for SIGINT, using same handler as SIGALRM to avoid conflict
static int setupSIGINT(void) {
	struct sigaction sigIntAct;
	sigIntAct.sa_handler = interruptHandler;
	sigIntAct.sa_flags = 0;
	sigemptyset(&sigIntAct.sa_mask);
	return (sigaction(SIGINT, &sigIntAct, NULL));
}

// sets ups itimer with time of 5s and interval of 0s
static int setupitimer() {
	struct itimerval value = { {0, 0}, {5, 0} };
	return (setitimer(ITIMER_REAL, &value, NULL));
}

// sets up timer, and SIGALRM and SIGINT handlers
static int setupInterrupts() {
	if (setupitimer() == -1) {
		perror("oss: Error");
		exit(-1);
	}
	if (setupAlarmInterrupt() == -1) {
		perror("oss: Error");
		exit(-1);
	}
	if (setupSIGINT() == -1) {
		perror("oss: Error");
		exit(-1);
	}
}

// displays frame allocation
void displayFrameMap(){
	int i, j;
	printf("\nFrame allocation:\n");
	for (i = 0; i < 16; i++) {
		for (j = 0; j < 16; j++) {
			printf("%3d ", 16 * i + j);
			if (frameMap[16 * i + j] == 1) { printf("+ "); }
			else { printf(". "); }
		}
		printf("\n");
	}
}

// handles process termination, releasing any allocated frames
void terminateProc(int pid){
	int status, i;

	printf("OSS: P%d terminated, freeing frames:\n\t", pid);
	for (i = 0; i < 32; i++) {
		if (shmptr->pageTables[pid][i].validBit == 1) {
			shmptr->frameTable[shmptr->pageTables[pid][i].frameNum] = (struct frame) { -1, -1, 0, 0, 0 };
			frameMap[shmptr->pageTables[pid][i].frameNum] = 0;
			printf("  %d  ", shmptr->pageTables[pid][i].frameNum);
			shmptr->pageTables[pid][i] = (struct page) { -1, 0 };
		}
	}
	printf("\n");
	numTermed++;
	shmptr->PIDmap[pid] = 0;
	mWait(&status);
	currentChildren--;
}

// spawn a new child
void spawnChildProc() {
	int i;
	pid_t pid;

	// finds available pid for new process, sets corresponding index of PIDmap to 1, and increments totalProcs and currentChildren
	for (i = lastPID + 1; i < 18; i++) { if (shmptr->PIDmap[i] == 0) { break; } }
	if (i == 18) { for (i = 0; i < lastPID; i++) { if (shmptr->PIDmap[i] == 0) { break; } } }
	shmptr->PIDmap[i] = 1;
	lastPID = i;
	currentChildren++;
	totalProcs++;

	// fork
	pid = fork();

	// rolls values back if fork fails
	if (pid == -1) {
		shmptr->PIDmap[i] = 0;
		currentChildren--;
		totalProcs--;
		perror("oss: fork Error");
	}

	// exec child
	else if (pid == 0) {
		char index[2];
		sprintf(index, "%d", i);
		execl("user_proc", index, (char*)NULL);
		exit(0);
	}

	else { printf("OSS: generating P%d at time %f s\n", i, timeToDouble(shmptr->currentTime)); }
}

void swapper() {
	struct pageRequest pageReq;
	struct msgbuf buf;
	int i;
	static int frameNum = -1;
	if (!isEmpty(pQueue)) {
		if (frameNum == -1) {
			for (i = 0; i < 256; i ++) { if (frameMap[i] == 0) { break; } }
			if (i < 256) {
				pageReq = dequeue(pQueue);
				frameMap[i] = 1;
				
				shmptr->frameTable[i] = (struct frame) { pageReq.pid, pageReq.address >> 10, 1, 1, 0 };
				printf("OSS: putting P%d page %d in free frame %d\n", pageReq.pid, pageReq.address >> 10, i);
				shmptr->pageTables[pageReq.pid][pageReq.address >> 10] = (struct page) { i, 1 };
				shmptr->stats.numReferences++;
				
				buf = (struct msgbuf) { pageReq.pid + 1, (struct msgInfo) { 20, pageReq.address, confirm }};
				if (msgsnd(msqid, &buf, sizeof(struct msgInfo), 0) == -1) { perror("OSS: error"); } 
			}
			else {
				// LRU ALGORITHM HERE?
				frameNum = rand() % 256;
				if (shmptr->frameTable[frameNum].dirtyBit == 1) {
					printf("OSS: saving frame %d to disk\n", frameNum);
					return;
				}
			}
		}
		if (frameNum > -1) {
			pageReq = dequeue(pQueue);
			shmptr->pageTables[shmptr->frameTable[frameNum].pid][shmptr->frameTable[frameNum].pageNum] = (struct page) { -1, 0 };
			printf("OSS: clearing frame %d and swapping in P%d page %d at %f s\n", frameNum, pageReq.pid, pageReq.address >> 10, timeToDouble(shmptr->currentTime));
			shmptr->stats.numReferences++;
			shmptr->frameTable[frameNum] = (struct frame) { pageReq.pid, pageReq.address >> 10, 1, 1, 0 };
			shmptr->pageTables[pageReq.pid][pageReq.address >> 10] = (struct page) { frameNum, 1 };
			
			buf = (struct msgbuf) { pageReq.pid + 1, (struct msgInfo) { 20, pageReq.address, confirm }};
			if (msgsnd(msqid, &buf, sizeof(struct msgInfo), 0) == -1) { perror("OSS: error"); } 
			frameNum = -1;
		}
	}
}

int main(int argc, char* argv[]) {
	int randomWait, i, j, opt;
	const int PROCMAX = 40;
	struct mtime oneSec = { 1, 0 };
	struct mtime timeToNextProc = { 0, rand() % (BILLION / 2 - 1000000) + 1000000};
	struct msgbuf buf;

	// initialize globals, interrupts, file pointer, and shared memory
	currentChildren = totalProcs = 0;
	xVal = 1;
	lastPID = -1;
	setupInterrupts();
	createMemory();
	setupFile();
	srand(time(0));
	pQueue = createQueue();
	shmptr->stats = (struct statistics) { { 0, 0 }, { 0, 0 }, 0, 0 };

	// parses command line arguments
    while ((opt = getopt(argc, argv, "m:")) != -1) { 
		if (opt == 'm') { 
			xVal = atoi(optarg); 
		}
		if (xVal > 2 || xVal < 1) {
			printf("OSS: Error: Improper value for command line argument m\n");
			exit(-1);
		}
	}

	// initialize PIDmap, frameMap, frameTable, pageTables, and currentTime
	for (i = 0; i < 18; i++) { shmptr->PIDmap[i] = 0; }
	for (i = 0; i < 256; i++) { 
		frameMap[i] = 0; 
		shmptr->frameTable[i] = (struct frame) { -1, -1, 0, 0, 0 };	
	}
	for (i = 0; i < 18; i++) {
		for (j = 0; j < 32; j++) {
			shmptr->pageTables[i][j] = (struct page) { -1, 0 };
		}
	}
	shmptr->currentTime.sec = shmptr->currentTime.ns = 0;

	// runs OSS until 40 processes have been spawned, and then until all children have terminated
	while (totalProcs < PROCMAX || currentChildren > 0) {
		// update shared clock
		shmptr->currentTime = addTime(shmptr->currentTime, 0, 250000);
		struct mtime IO = { 0, 14*MILLION };

		// display frame allocation every second
		if (compareTimes(shmptr->currentTime, oneSec)) {
			displayFrameMap();
			oneSec = addTime(oneSec, 1, 0);
		}

		// spawns new process if process table isn't full and PROCMAX hasn't been reached
		if (compareTimes(shmptr->currentTime, timeToNextProc) && currentChildren < 18 && totalProcs < PROCMAX) {
			spawnChildProc();
			timeToNextProc = addTime(shmptr->currentTime, 0, rand() % (BILLION / 2 - 1000000) + 1000000);
		}

		// read any messages for OSS, but do not wait for one
		while (msgrcv(msqid, &buf, sizeof(struct msgInfo), 20, IPC_NOWAIT) >= 0) {
			if (buf.info.act == readReq) {
				printf("OSS: P%d requesting read of address %d at %f s\n", buf.info.pid, buf.info.address, timeToDouble(shmptr->currentTime));
				if (shmptr->pageTables[buf.info.pid][buf.info.address >> 10].validBit == 1) {
					shmptr->currentTime = addTime(shmptr->currentTime, 0, 10);
					printf("OSS: address %d in frame %d, giving data to P%d\n",
						buf.info.address, shmptr->pageTables[buf.info.pid][buf.info.address >> 10].frameNum, buf.info.pid);
					shmptr->stats.numReferences++;
					buf = (struct msgbuf) { buf.info.pid + 1, (struct msgInfo) { 20, buf.info.address, confirm }};
					if (msgsnd(msqid, &buf, sizeof(struct msgInfo), 0) == -1) { perror("OSS: error"); } 
				}
				else {
					printf("OSS: address %d is not in a frame: page fault\n", buf.info.address);
					shmptr->stats.numPageFaults++;
					enqueue(pQueue, (struct pageRequest){ buf.info.pid, buf.info.address, readReq });
				}
			}
			else if (buf.info.act == writeReq) {

			}
			else if (buf.info.act == terminate) {
				terminateProc(buf.info.pid);
			}
		}

		// access disk if 14ms have passed since last access
		if (compareTimes(shmptr->currentTime, IO)) {
			swapper();
			IO = addTime(shmptr->currentTime, 0, 14 * MILLION);
		}
	}

	// finish up
	printf("\nOSS: 40 processes have been spawned and run to completion, now terminating OSS\n");
	terminateOSS();
}