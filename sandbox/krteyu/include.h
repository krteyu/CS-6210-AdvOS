#ifndef __INCLUDE_GUARD__
#define __INCLUDE_GUARD__
#include <sys/ipc.h>     // key_t, IPC_CREAT, IPC_PRIVATE, IPC_RMID
#include <sys/shm.h>     // shmget, shmat, shmdt, shmctl
#include <sys/sem.h>     // semget, semop, semctl
#include <cstdint>        // uint32_t, uint64_t etc.
#include <cstring>        // memcpy
#include <atomic>         // std::atomic for ring head/tail
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <fstream>
#endif