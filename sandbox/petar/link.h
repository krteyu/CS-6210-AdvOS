#ifndef __LINK_H__
#define __LINK_H__
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <unistd.h>
#include <atomic>
#include <cstring>
#include <csignal>
#include <iostream>
#include <fstream>
#include <string>

#define INFO_PATH "/tmp/petar_info.txt"
#define MAX_CLIENTS 8

struct Mailbox {
    std::atomic<bool> used;
    pid_t pid;
};

struct Region {
    int semid;
    Mailbox mbox[MAX_CLIENTS];
};

inline void lock(int semid) {
    sembuf op = {0, -1, SEM_UNDO};
    semop(semid, &op, 1);
}

inline void unlock(int semid) {
    sembuf op = {0, +1, SEM_UNDO};
    semop(semid, &op, 1);
}
#endif
