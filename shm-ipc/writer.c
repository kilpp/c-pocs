#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "shared.h"

int main(void) {
    // Create and size the shared memory object
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (fd == -1) { perror("shm_open"); return 1; }

    if (ftruncate(fd, sizeof(SharedData)) == -1) { perror("ftruncate"); return 1; }

    SharedData *shm = mmap(NULL, sizeof(SharedData),
                           PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) { perror("mmap"); return 1; }
    close(fd);

    // pshared=1 means semaphore is shared between processes (not threads)
    sem_init(&shm->sem_write, /*pshared=*/1, /*value=*/1); // writer goes first
    sem_init(&shm->sem_read,  /*pshared=*/1, /*value=*/0); // reader waits

    shm->total = MSG_COUNT;
    shm->index = 0;

    printf("[writer] shared memory ready: %s\n", SHM_NAME);
    printf("[writer] sending %d messages...\n\n", MSG_COUNT);

    for (int i = 0; i < MSG_COUNT; i++) {
        sem_wait(&shm->sem_write);  // wait for slot to be free

        snprintf(shm->message, MSG_SIZE, "hello from writer, message #%d", i + 1);
        shm->index = i + 1;

        printf("[writer] wrote: \"%s\"\n", shm->message);

        sem_post(&shm->sem_read);   // signal reader data is ready
        usleep(200 * 1000);         // 200ms pacing so output is readable
    }

    // Wait for reader to consume the last message before cleanup
    sem_wait(&shm->sem_write);

    printf("\n[writer] all messages sent, cleaning up\n");
    sem_destroy(&shm->sem_write);
    sem_destroy(&shm->sem_read);
    munmap(shm, sizeof(SharedData));
    shm_unlink(SHM_NAME);

    return 0;
}
