#pragma once

#include <semaphore.h>

#define SHM_NAME   "/poc_shm_ipc"
#define MSG_SIZE   256
#define MSG_COUNT  8

typedef struct {
    sem_t sem_write;        // writer waits on this before writing
    sem_t sem_read;         // reader waits on this before reading
    int   total;            // total messages to exchange
    int   index;            // current message index
    char  message[MSG_SIZE];
} SharedData;
