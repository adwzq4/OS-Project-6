// Author: Adam Wilson
// Date: 12/7/1941

#include <stdlib.h>
#include <limits.h>
#include <stdio.h> 
#include "shared.h"

// outputs possible errors for wait() call
void mWait(int* status) {
    if (wait(&status) > 0) {
        if (WIFEXITED(status) && WEXITSTATUS(status)) {
            if (WEXITSTATUS(status) == 127) { perror("oss: Error"); }
            else { perror("oss: Error"); }
        }
    }
}

// adds two mtime structs together
struct mtime addTime(struct mtime t1, int sec, long ns) {
    t1.sec += sec;
    t1.ns += ns;
    if (t1.ns >= BILLION) {
        t1.ns -= BILLION;
        t1.sec++;
    }
    return t1;
}

// subtracts the second mtime struct from the first
struct mtime subtractTime(struct mtime t1, struct mtime t2) {
    t1.sec -= t2.sec;
    t1.ns -= t2.ns;
    if (t1.ns < 0) {
        t1.ns += BILLION;
        t1.sec--;
    }
    return t1;
}

// compares two mtime structs, returning 0 if t1 < t2, and 1 if t1 >= t2
int compareTimes(struct mtime t1, struct mtime t2) {
    if (t1.sec < t2.sec || t1.sec == t2.sec && t1.ns < t2.ns) { return 0; }
    else { return 1; }
}

// converts an mtime struct to a double
double timeToDouble(struct mtime t) { return t.sec + (double)t.ns / BILLION; }

// creates queue with capacity of 20 and initial size of 0
struct pageQueue* createQueue() {
    struct pageQueue* queue = (struct pageQueue*)malloc(sizeof(struct pageQueue));
    queue->capacity = 20;
    queue->front = queue->size = 0;
    queue->rear = 19;
    queue->array = (struct pageRequest*)malloc(queue->capacity * sizeof(struct pageRequest));
    return queue;
}

// queue is full if size == capacity
int isFull(struct pageQueue* queue) { return (queue->size == queue->capacity); }

// queue is empty if size == 0 
int isEmpty(struct pageQueue* queue) { return (queue->size == 0); }

// adds item to rear of queue
void enqueue(struct pageQueue* queue, struct pageRequest item) {
    //if (isFull(queue)) { return; }
    queue->rear = (queue->rear + 1) % queue->capacity;
    queue->array[queue->rear] = item;
    queue->size = queue->size + 1;
}

// removes item from front of queue
struct pageRequest dequeue(struct pageQueue* queue) {
    //if (isEmpty(queue)) { return INT_MIN; }
    struct pageRequest item = queue->array[queue->front];
    queue->front = (queue->front + 1) % queue->capacity;
    queue->size = queue->size - 1;
    return item;
}
