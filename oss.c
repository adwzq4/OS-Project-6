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
int msqid, shmid, currentChildren, totalProcs, lastPID, xVal, tVal;
int frameMap[256];
struct shmseg* shmptr;
struct pageQueue* pQueue;
struct pageQueue* fQueue;
int numTermed = 0, saved = 0;

// creates a shared memory segment, a message queue, and a semaphore
void createMemory() {
	key_t shmkey, msqkey;
	
	shmkey = ftok("oss", 137);
	shmid = shmget(shmkey, sizeof(struct shmseg), 0666 | IPC_CREAT);
	if (shmid == -1) {
		perror("oss: Error");
		exit(-1);
	}

	shmptr = shmat(shmid, (void*)0, 0);
	if (shmptr == (void*)-1) { perror("oss: Error"); }

	msqkey = ftok("oss", 731);
	msqid = msgget(msqkey, 0666 | IPC_CREAT);
	if (msqid == -1) { perror("oss: Error"); }
}

// outputs stats, waits for children, destroys message queue, and detaches and destroys shared memory
void terminateOSS() {
	int i, j, status;
	fprintf(fp, "\n\nOSS ran for %.4f s\n", timeToDouble(shmptr->currentTime));
	fprintf(fp, "Total references: %d\n", shmptr->stats.numReferences);
	fprintf(fp, "Memory references: %.3f / s\n", shmptr->stats.numReferences / timeToDouble(shmptr->currentTime));
	fprintf(fp, "Page faults: %.3f / memory access\n", shmptr->stats.numPageFaults / (double)shmptr->stats.numReferences);
	fprintf(fp, "Processes completed: %d\n", numTermed);
	fprintf(fp, "Number of frames saved to disk: %d\n", saved);
	fclose(fp);
	for (i = 0; i < currentChildren; i++) { mWait(&status); }
	if (msgctl(msqid, IPC_RMID, NULL) == -1) { perror("oss: msgctl"); }
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

// sets ups itimer with default time of 2s, which can be changed by -t parameter, and interval of 0s
static int setupitimer() {
	struct itimerval value = { {0, 0}, {tVal, 0} };
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

// displays frame allocation: a + indicates occupied, while a . indicates unoccupied
void displayFrameMap(){
	int i, j;
	fprintf(fp, "\nFrame allocation:\n");
	for (i = 0; i < 16; i++) {
		for (j = 0; j < 16; j++) {
			fprintf(fp, "%3d ", 16 * i + j);
			if (frameMap[16 * i + j]) { fprintf(fp, "+ "); }
			else { fprintf(fp, ". "); }
		}
		fprintf(fp, "\n");
	}
	fprintf(fp, "\n");
}

// handles process termination, checking the page table any frames allocated to the process and releasing them
void terminateProc(int pid) {
	int status, i, frames = 0, dirty = 0;

	for (i = 0; i < 32; i++) {
		if (shmptr->pageTables[pid][i].validBit == 1) {
			// if dirty bit of a page is set, enqueue it to the disk write queue
			if (shmptr->frameTable[shmptr->pageTables[pid][i].frameNum].dirtyBit == 1) { 
				saved++;
				dirty++;
				enqueue(fQueue, (struct pageRequest) { shmptr->pageTables[pid][i].frameNum, -1, save });
			}
			shmptr->frameTable[shmptr->pageTables[pid][i].frameNum] = (struct frame) { -1, -1, 0, 0, 0 };
			frameMap[shmptr->pageTables[pid][i].frameNum] = 0;
			frames++;
			shmptr->pageTables[pid][i] = (struct page) { -1, 0 };
		}
	}

	// display info about process termination, including effective memory access time
	fprintf(fp, "OSS: P%d terminated, freeing %d frames, %d of which need to be saved to disk;\n\tits effective memory access time was %d us / memory request\n",
		pid, frames, dirty, (int) (timeToDouble(shmptr->stats.processAccessTimes[pid]) * MILLION) / shmptr->stats.processRerences[pid]);
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

	// exec child with pid and xVal as parameters
	else if (pid == 0) {
		char index[10];
		char x[10];
		sprintf(index, "%d", i);
		sprintf(x, "%d", xVal);
		execl("user_proc", index, x, (char*)NULL);
		exit(0);
	}

	else { fprintf(fp, "OSS: generating P%d at time %f s\n", i, timeToDouble(shmptr->currentTime)); }
}

void swapper() {
	struct pageRequest pageReq;
	struct msgbuf buf;
	int i, min = 257;
	static int frameNum = -1;
	// if any frames are waiting to be saved, dequeue the first one and indicate so, consuming 14ms
	if (!isEmpty(fQueue)) { 
		fprintf(fp, "OSS: saving frame %d to disk at %f s\n", dequeue(fQueue).pid, timeToDouble(shmptr->currentTime));
		for (i = 0; i < pQueue->size; i++) {
			shmptr->stats.processAccessTimes[pQueue->array[i].pid] = addTime(shmptr->stats.processAccessTimes[pQueue->array[i].pid], 0, 14 * MILLION);
		}
	}
	// otherwise, access the disk read queue if it has any page requests
	else if (!isEmpty(pQueue)) {
		// if frameNum is unset, scan frameMap for an available frame
		if (frameNum == -1) {
			for (i = 0; i < 256; i++) { if (frameMap[i] == 0) { break; } }
			// if there is an unoccupied frame, dequeue a page request, update frame map, and indicate there was a free frame; frameNum stays unset
			if (i < 256) {
				pageReq = dequeue(pQueue);
				frameMap[i] = 1;
				fprintf(fp, "OSS: putting P%d page %d in free frame %d at %f s\n\t", pageReq.pid, pageReq.address >> 10, i, timeToDouble(shmptr->currentTime));
			}
			// if all frames are occupied, run LRU algorithm with dirty bit optimization
			else {
				// find minimum refByte of frames whose dirtyBits are unset and assign index to frameNum 
				for (i = 0; i < 256; i++) {
					if (shmptr->frameTable[i].refByte < min && shmptr->frameTable[i].dirtyBit == 0) {
						min = shmptr->frameTable[i].refByte;
						frameNum = i;
					}
				}
				// if all dirtyBits are set, find minimum refByte, assign index to static var frameNum, enqueue frame to disk write queue,
				// and return from function, consuming 14ms
				if (min == 257) {
					for (i = 0; i < 256; i++) {
						if (shmptr->frameTable[i].refByte < min) {
							min = shmptr->frameTable[i].refByte;
							frameNum = i;
						}
					}
					enqueue(fQueue, (struct pageRequest) { frameNum, -1, save });
					saved++;
					return;
				}
			}
		}
		// if frameNum is set, dequeue a page request, reset the corresponding page table entry, write the swap to the log, and unset frameNum
		if (frameNum > -1) {
			pageReq = dequeue(pQueue);
			shmptr->pageTables[shmptr->frameTable[frameNum].pid][shmptr->frameTable[frameNum].pageNum] = (struct page){ -1, 0 };
			fprintf(fp, "OSS: clearing frame %d and swapping in P%d page %d at %f s,\n\t", frameNum, pageReq.pid, pageReq.address >> 10, timeToDouble(shmptr->currentTime));
			i = frameNum;
			frameNum = -1;
		}

		// put the requested page into the assigned frame, setting refByte to the max value and dirtyBit to 0, and update page table
		shmptr->pageTables[pageReq.pid][pageReq.address >> 10] = (struct page){ i, 1 };
		shmptr->frameTable[i] = (struct frame){ pageReq.pid, pageReq.address >> 10, 1, 256, 0 };
		// write to log whether a read or write occurred, and in the latter case set the corresponding frame's dirty bit to 1
		if (pageReq.act == readReq) { fprintf(fp, "then giving data at address %d to P%d\n", pageReq.address, pageReq.pid); }
		if (pageReq.act == writeReq) {
			fprintf(fp, "then indicating to P%d that address %d was written to\n", pageReq.pid, pageReq.address);
			shmptr->frameTable[i].dirtyBit = 1;
		}

		// update stats
		shmptr->stats.numReferences++;
		shmptr->stats.processRerences[pageReq.pid]++;
		shmptr->stats.processAccessTimes[pageReq.pid] = addTime(shmptr->stats.processAccessTimes[pageReq.pid], 0, 14 * MILLION);
		for (i = 0; i < pQueue->size; i++) {
			shmptr->stats.processAccessTimes[pQueue->array[i].pid] = addTime(shmptr->stats.processAccessTimes[pQueue->array[i].pid], 0, 14 * MILLION);
		}

		// send confirmation to waiting child process
		buf = (struct msgbuf){ pageReq.pid + 1, (struct msgInfo) { 20, pageReq.address, confirm } };
		if (msgsnd(msqid, &buf, sizeof(struct msgInfo), 0) == -1) { perror("OSS: error"); }
	}
}

// spawns processes which request memory addresses, oss grants access via paging
int main(int argc, char* argv[]) {
	int randomWait, i, j, opt;
	const int PROCMAX = 40;
	struct msgbuf buf;
	// initialize clocks
	struct mtime oneSec = { 1, 0 };
	struct mtime timeToNextProc = { 0, rand() % (BILLION / 2 - 1000000) + 1000000};
	struct mtime IO = { 0, 14 * MILLION };
	struct mtime updateRefBytes = { 0, 100 * MILLION };

	// parses command line arguments
	xVal = 1, tVal = 2;
	while ((opt = getopt(argc, argv, "m:t:")) != -1) {
		if (opt == 'm') {
			xVal = atoi(optarg);
			if (xVal > 2 || xVal < 1) {
				printf("OSS: Error: Improper value for command line argument m\n");
				exit(-1);
			}
		}
		else if (opt == 't') {
			tVal = atoi(optarg);
			if (tVal < 1 || tVal > 50) {
				printf("OSS: Error: Improper value for command line argument t\n");
				exit(-1);
			}
		}
	}

	// initialize globals, interrupts, file pointer, and shared memory
	currentChildren = totalProcs = 0;
	lastPID = -1;
	setupInterrupts();
	createMemory();
	setupFile();
	srand(time(0));
	pQueue = createQueue();
	fQueue = createQueue();

	// initialize PIDmap, frameMap, frameTable, pageTables, stats, and shared/main clock
	shmptr->stats.numPageFaults = shmptr->stats.numReferences = 0;
	for (i = 0; i < 18; i++) { 
		shmptr->stats.processAccessTimes[i] = (struct mtime){ 0, 0 };
		shmptr->stats.processRerences[i] = 0;
		shmptr->PIDmap[i] = 0; 
		for (j = 0; j < 32; j++) {
			shmptr->pageTables[i][j] = (struct page){ -1, 0 };
		}
	}
	for (i = 0; i < 256; i++) { 
		frameMap[i] = 0; 
		shmptr->frameTable[i] = (struct frame) { -1, -1, 0, 0, 0 };	
	}
	shmptr->currentTime.sec = shmptr->currentTime.ns = 0;
	
	// runs OSS until 40 processes have been spawned, and then until all children have terminated
	while (totalProcs < PROCMAX || currentChildren > 0) {
		// update shared clock, skip to next disk IO if page request queue is full
		shmptr->currentTime = addTime(shmptr->currentTime, 0, 7*MILLION);
		if (pQueue->size == 18) shmptr->currentTime = addTime(shmptr->currentTime, 0, 7*MILLION);
		else {
			// spawns new process if process table isn't full and PROCMAX hasn't been reached
			if (compareTimes(shmptr->currentTime, timeToNextProc) && currentChildren < 18 && totalProcs < PROCMAX) {
				spawnChildProc();
				timeToNextProc = addTime(shmptr->currentTime, 0, rand() % (BILLION / 2 - 1000000) + 1000000);
			}

			// read any messages for OSS, but do not wait for one
			while (msgrcv(msqid, &buf, sizeof(struct msgInfo), 20, IPC_NOWAIT) >= 0) {
				if (buf.info.act == terminate) {
					terminateProc(buf.info.pid);
					continue;
				}
				
				// display time at which a process is requesting a read or a write
				if (buf.info.act == readReq) fprintf(fp, "OSS: P%d requesting read of address %d at %f s\n", buf.info.pid, buf.info.address, timeToDouble(shmptr->currentTime));
				else fprintf(fp, "OSS: P%d requesting write to address %d at %f s\n", buf.info.pid, buf.info.address, timeToDouble(shmptr->currentTime));
				
				// if valid bit is set on the corresponding page table entry, requested page is in a frame; add 10ns and indicate page hit
				if (shmptr->pageTables[buf.info.pid][buf.info.address >> 10].validBit == 1) {
					shmptr->currentTime = addTime(shmptr->currentTime, 0, 10);
					if (buf.info.act == readReq) fprintf(fp, "OSS: address %d in frame %d, giving data to P%d\n",
							buf.info.address, shmptr->pageTables[buf.info.pid][buf.info.address >> 10].frameNum, buf.info.pid);
					else fprintf(fp, "OSS: address %d in frame %d, indicating to P%d that write has occurred\n",
							buf.info.address, shmptr->pageTables[buf.info.pid][buf.info.address >> 10].frameNum, buf.info.pid);
					
					// update stats
					shmptr->stats.numReferences++;
					shmptr->stats.processRerences[buf.info.pid]++;
					shmptr->stats.processAccessTimes[buf.info.pid] = addTime(shmptr->stats.processAccessTimes[buf.info.pid], 0, 10);
					for (i = 0; i < pQueue->size; i++) {
						shmptr->stats.processAccessTimes[pQueue->array[i].pid] = addTime(shmptr->stats.processAccessTimes[pQueue->array[i].pid], 0, 10);
					}

					// set refByte to 256, and set dirtyBit if this is a write request
					if (buf.info.act == writeReq) { shmptr->frameTable[shmptr->pageTables[buf.info.pid][buf.info.address >> 10].frameNum].dirtyBit = 1; }
					shmptr->frameTable[shmptr->pageTables[buf.info.pid][buf.info.address >> 10].frameNum].refByte = 256;
					
					// send confirmation to waiting child process
					buf = (struct msgbuf){ buf.info.pid + 1, (struct msgInfo) { 20, buf.info.address, confirm } };
					if (msgsnd(msqid, &buf, sizeof(struct msgInfo), 0) == -1) { perror("OSS: error"); }
				}
				// otherwise there is a page fault; enqueue the page request to the disk read queue
				else {
					fprintf(fp, "OSS: address %d is not in a frame: page fault\n", buf.info.address);
					shmptr->stats.numPageFaults++;
					if (buf.info.act == readReq) { enqueue(pQueue, (struct pageRequest) { buf.info.pid, buf.info.address, readReq }); }
					else { enqueue(pQueue, (struct pageRequest) { buf.info.pid, buf.info.address, writeReq }); }
				}
			}
		}

		// right bit shift the refBytes of all frames by 1 every 100ms
		if (compareTimes(shmptr->currentTime, updateRefBytes)) {
			for (i = 0; i < 256; i++) { shmptr->frameTable[i].refByte = shmptr->frameTable[i].refByte >> 1; }
			updateRefBytes = addTime(shmptr->currentTime, 0, 100 * MILLION);
		}
		
		// access disk if 14ms have passed since last access
		if (compareTimes(shmptr->currentTime, IO)) {
			swapper();
			IO = addTime(shmptr->currentTime, 0, 14 * MILLION);
		}

		// display frame allocation every second
		if (compareTimes(shmptr->currentTime, oneSec)) {
			displayFrameMap();
			oneSec = addTime(oneSec, 1, 0);
		}
	}

	// finish up
	//for (i = 0; i < fQueue->size; i++) {
	//	shmptr->currentTime = addTime(shmptr->currentTime, 0, 14 * MILLION);
	//	printf("OSS: saving frame %d to disk at %f s\n", dequeue(fQueue).pid, timeToDouble(shmptr->currentTime));
	//}

	printf("\nOSS: 40 processes have been spawned and run to completion, now terminating OSS\n\n");
	terminateOSS();
}
