#define _GNU_SOURCE

#include "io.h"
#include "message.h"
#include "ring.h"

#include <alloca.h>
#include <bits/pthreadtypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

struct Worker {
  int running;
  pthread_t thread;
};

enum WorkerType { PRODUCER, CONSUMER };

int producerCount = 0;
struct Worker *producers = NULL;

int consumerCount = 0;
struct Worker *consumers = NULL;

thread_local int workerIndex;
thread_local enum WorkerType workerType;

void Worker_initSelf(enum WorkerType type) {
  switch (type) {
  case PRODUCER: workerIndex = producerCount - 1; break;
  case CONSUMER: workerIndex = consumerCount - 1; break;
  }
  workerType = type;
}

int Worker_running() {
  switch (workerType) {
  case PRODUCER: return producers[workerIndex].running;
  case CONSUMER: return consumers[workerIndex].running;
  default: return 0;
  }
}

struct Shared {
  pthread_mutex_t general;
  int sendCount;
  int readCount;
  struct Ring *ring;
} *shared = NULL;

void initShared(int ringCapacity) {
  shared = malloc(sizeof(struct Shared));
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutex_init(&shared->general, &attr);
  shared->sendCount = 0;
  shared->readCount = 0;
  shared->ring =
      Ring_construct(malloc(sizeof(struct Ring) + ringCapacity), ringCapacity);
}

void destroyShared() {
  Ring_desctruct(shared->ring);
  pthread_mutex_destroy(&shared->general);
  free(shared);
}

void *producer() {
  Worker_initSelf(PRODUCER);
  printf("Producer %6d Started\n", getpid());
  while (Worker_running()) {
    sleep(1);
    pthread_mutex_lock(&shared->general);
    char bytes[MESSAGE_MAX_SIZE] = {0};
    struct Message *message = Message_constructRandom((struct Message *)bytes);
    while (Worker_running()) {
      pthread_mutex_unlock(&shared->general);
      pthread_mutex_lock(&shared->general);
      if (Message_sendTo(message, shared->ring) == -1) continue;
      shared->sendCount++;
      char data[255 * 3] = {0};
      bytes2hex(data, message->size, message->data);
      printf(
          "Producer %6d Sent %04hX:%04hX       %.80s\n",
          getpid(),
          message->type,
          message->hash,
          data
      );
      break;
    }
    pthread_mutex_unlock(&shared->general);
  }
  printf("Producer %6d Stopped\n", getpid());
  return NULL;
}

void *consumer() {
  Worker_initSelf(CONSUMER);
  printf("Consumer %6d Started\n", getpid());
  while (Worker_running()) {
    pthread_mutex_lock(&shared->general);
    while (Worker_running()) {
      pthread_mutex_unlock(&shared->general);
      pthread_mutex_lock(&shared->general);
      char bytes[MESSAGE_MAX_SIZE] = {0};
      struct Message *message =
          Message_readFrom((struct Message *)bytes, shared->ring);
      if (!message) continue;
      shared->readCount++;
      char data[255 * 3] = {0};
      bytes2hex(data, message->size, message->data);
      printf(
          "Consumer %6d Got  %04hX:%04hX(%04hX) %.80s\n",
          getpid(),
          message->type,
          message->hash,
          Message_hash(message),
          data
      );
      break;
    }
    pthread_mutex_unlock(&shared->general);
    sleep(1);
  }
  printf("Consumer %6d Stopped\n", getpid());
  return NULL;
}

void setSharedRingCapacity(int capacity) {
  pthread_mutex_lock(&shared->general);
  struct Ring *newRing =
      Ring_construct(malloc(sizeof(struct Ring) + capacity), capacity);
  if (Ring_pour(shared->ring, newRing) == 0) {
    struct Ring *temp = shared->ring;
    shared->ring = newRing;
    newRing = temp;
  }
  Ring_desctruct(newRing);
  free(newRing);
  pthread_mutex_unlock(&shared->general);
  printf("Ring capacity %d\n", shared->ring->capacity);
}

typedef int (*handle_f)();

int showInfo() {
  printf(
      "Sent %d(%d) Got %d(%d)\n",
      shared->sendCount,
      producerCount,
      shared->readCount,
      consumerCount
  );
  return 0;
}

int addProducer() {
  producerCount++;
  producers = realloc(producers, sizeof(*producers) * producerCount);
  struct Worker *worker = &producers[producerCount - 1];
  worker->running = 1;
  pthread_create(&worker->thread, NULL, producer, NULL);
  return 0;
}

int killProducer() {
  if (producerCount == 0) return 0;
  struct Worker *worker = &producers[producerCount - 1];
  worker->running = 0;
  pthread_join(worker->thread, NULL);
  producerCount--;
  return 0;
}

int addConsumer() {
  consumerCount++;
  consumers = realloc(consumers, sizeof(*consumers) * consumerCount);
  struct Worker *worker = &consumers[consumerCount - 1];
  worker->running = 1;
  pthread_create(&worker->thread, NULL, consumer, NULL);
  return 0;
}

int killConsumer() {
  if (consumerCount == 0) return 0;
  struct Worker *worker = &consumers[consumerCount - 1];
  worker->running = 0;
  pthread_join(worker->thread, NULL);
  consumerCount--;
  return 0;
}

int increaseRingCapacity() {
  setSharedRingCapacity(shared->ring->capacity * 2);
  return 0;
}

int decreaseRingCapacity() {
  setSharedRingCapacity(shared->ring->capacity / 2);
  return 0;
}

int quit() { return -1; }

int unknownCommand() { return 0; }

handle_f handleFor(char key) {
  switch (key) {
  case 'i': return showInfo;
  case 'p': return addProducer;
  case 'P': return killProducer;
  case 'c': return addConsumer;
  case 'C': return killConsumer;
  case '+': return increaseRingCapacity;
  case '-': return decreaseRingCapacity;
  case 'q': return quit;
  default: return unknownCommand;
  }
}

int main() {
  initShared(1024);
  while (handleFor(getch())() == 0)
    ;
  while (producerCount)
    killProducer();
  while (consumerCount)
    killConsumer();
  destroyShared();
}
