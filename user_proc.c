// Author: Adam Wilson
// Date: 12/2/2020

#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h> 
#include <sys/shm.h> 
#include <sys/types.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include "shared.h"

int msqid, shmid;
struct shmseg* shmptr;

void attachToSharedMemory(){
	key_t shmkey, msqkey;
	// create shared memory segment the same size as struct shmseg and get its shmid
	shmkey = ftok("oss", 137);
	shmid = shmget(shmkey, sizeof(struct shmseg), 0666 | IPC_CREAT);
	if (shmid == -1) {
		perror("user_proc: Error");
		exit(-1);
	}

	// attach struct pointer to shared memory segment and seed rand() with pid * time
	shmptr = shmat(shmid, (void*)0, 0);
	if (shmptr == (void*)-1) { perror("user_proc: Error"); }

	// attach to same message queue as parent
	msqkey = ftok("oss", 731);
	msqid = msgget(msqkey, 0666 | IPC_CREAT);
	if (msqid == -1) { perror("user_proc: Error"); }
}

// simulates an instance of a process spawned by OSS
int main(int argc, char* argv[]) {
	struct msgbuf buf;
	struct mtime initTime, waitTil;
	int i, j, term, pid, xVal = 1;
	const int TERMRATIO = 2;
	float weightedArray[32];
	
	// get pid and xVal from execl parameters and set up shared memory/message queue
	pid = atoi(argv[0]);
	xVal = atoi(argv[1]);
	attachToSharedMemory();
	srand(pid * shmptr->currentTime.ns);
	
	// check whether to terminate after 1000 +/- 100 total references
	term = shmptr->stats.numReferences + rand() % 200 + 900;

	// if 1/n option invoked, create array of summations of 1/i from i=1 to [k=1, k=2, ... k=32]
	if (xVal == 2) {
		for (i = 0; i < 32; i++) {
			weightedArray[i] = 1. / (i + 1);
			if (i > 0) { weightedArray[i] += weightedArray[i - 1]; }
		}
	}

	// until the process terminates, it will continue to request addresses
	while (1) {
		// always send messages to oss (type 20) and always send this process's pid
		buf.type = 20;
		buf.info.pid = pid;
		
		// wait between 0-1ms
		waitTil = addTime(shmptr->currentTime, 0, rand() % MILLION);
		while (!compareTimes(shmptr->currentTime, waitTil));

		// if enough requests have been completed, there is a 50% chance of termination, where usere_proc sends a
		// message indicating this to oss and breaks the loop; otherwise, it checks again after 100-200 more requests
		if (shmptr->stats.numReferences >= term) {
			if (rand() % TERMRATIO == 0) {
				buf.info.act = terminate;
				buf.info.address = -1;
				if (msgsnd(msqid, &buf, sizeof(struct msgInfo), 0) == -1) { perror("user_proc: Error"); }
				break;
			}
			else term += 100 + rand() % 100;
		}

		// requests a read 80% of the time and a write 20% of the time
		int r = rand() % 10;
		if (r < 8) { buf.info.act = readReq; }
		else { buf.info.act = writeReq;  }

		// if -m 1 is used, requests a random address from 0-32768 
		if (xVal == 1) { buf.info.address = rand() % 32768; }
		// if -m 2 is used, gets a random number from 0-4.0585, finds the max index of weightedArray whose value is
		// less than that number, left bitshifts that index by 10 to get the page number, then adds a random offset
		else if (xVal == 2) {
			for (i = 0; rand() % (int)(weightedArray[31]*10000) / 10000. > weightedArray[i]; i++);
			buf.info.address = i * 1024 + rand() % 1024;
		}

		// sends the address request to oss, then waits until a confirmation message is received
		if (msgsnd(msqid, &buf, sizeof(struct msgInfo), 0) == -1) { perror("user_proc: Error"); }
		if (msgrcv(msqid, &buf, sizeof(struct msgInfo), buf.info.pid + 1, 0) == -1) { perror("user_proc: Error"); }
	}

	// detaches shmseg from shared memory
	if (shmdt(shmptr) == -1) {
		perror("user_proc: Error");
		exit(-1);
	}
}
