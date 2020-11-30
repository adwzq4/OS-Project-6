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

int msqid, shmid, semid;
//union semun sem;
//struct sembuf p = { 0, -1, SEM_UNDO };
//struct sembuf v = { 0, +1, SEM_UNDO };
struct shmseg* shmptr;

void attachToSharedMemory(){
	key_t shmkey, msqkey, semkey;
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

	//// create semaphore with specified key
 //   semkey = ftok("oss", 484);
 //   semid = semget(semkey, 1, 0666 | IPC_CREAT);
 //   if (semid < 0) { perror("semget"); }
}

// simulates an instance of a process spawned by OSS
int main(int argc, char* argv[]) {
	struct msgbuf buf;
	struct mtime initTime, waitTil;
	int i, j, term, pid;
	const int TERMRATIO = 2;
	
	// get pid from execl parameter and set up shared memory/semaphore/message queue
	pid = atoi(argv[0]);
	attachToSharedMemory();
	srand(pid * shmptr->currentTime.ns);
	initTime = shmptr->currentTime;
	term = shmptr->stats.numReferences + rand() % 200 + 900;

	// until the process terminates, it will continue to request addresses
	while (1) {
		buf.type = 20;
		buf.info.pid = pid;
		
		// wait between 0-1ms
		waitTil = addTime(shmptr->currentTime, 0, rand() % MILLION);
		while (!compareTimes(shmptr->currentTime, waitTil));

		if (shmptr->stats.numReferences >= term) {
			if (rand() % TERMRATIO == 0) {
				buf.info.act = terminate;
				buf.info.address = -1;
				if (msgsnd(msqid, &buf, sizeof(struct msgInfo), 0) == -1) { perror("user_proc: Error"); }
				break;
			}
			else term += rand() % 50;
		}

		if (rand() % 10 >= 8) { buf.info.act = readReq; }
		else { buf.info.act = writeReq;  }
		buf.info.address = rand() % 32768;
		
		if (msgsnd(msqid, &buf, sizeof(struct msgInfo), 0) == -1) { perror("user_proc: Error"); }
		if (msgrcv(msqid, &buf, sizeof(struct msgInfo), buf.info.pid + 1, 0) == -1) { perror("user_proc: Error"); }
	}

	// detaches shmseg from shared memory
	if (shmdt(shmptr) == -1) {
		perror("user_proc: Error");
		exit(-1);
	}
}
