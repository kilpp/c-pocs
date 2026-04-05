#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "shared.h"

int main(void) {
    // Writer must have created the shm object already
    int fd = shm_open(SHM_NAME, O_RDWR, 0);
    if (fd == -1) {
        perror("shm_open (did you start the writer first?)");
        return 1;
    }

    SharedData *shm = mmap(NULL, sizeof(SharedData),
                           PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) { perror("mmap"); return 1; }
    close(fd);

    printf("[reader] attached to shared memory: %s\n", SHM_NAME);
    printf("[reader] waiting for messages...\n\n");

    int total = shm->total;

    for (int i = 0; i < total; i++) {
        sem_wait(&shm->sem_read);   // block until writer posts data

        printf("[reader] read (#%d/%d): \"%s\"\n", shm->index, total, shm->message);

        sem_post(&shm->sem_write);  // signal writer the slot is free
    }

    printf("\n[reader] done, received all %d messages\n", total);
    munmap(shm, sizeof(SharedData));

    return 0;
}
