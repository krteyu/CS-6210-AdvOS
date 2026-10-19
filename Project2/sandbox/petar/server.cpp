#include "link.h"
using namespace std;

volatile sig_atomic_t running = 1;
void stop(int) { running = 0; }

int main() {
    int shm_id = shmget(IPC_PRIVATE, sizeof(Region), IPC_CREAT | 0600);
    Region *region = (Region *)shmat(shm_id, nullptr, 0);
    memset(region, 0, sizeof(Region));

    int sem_id = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    semctl(sem_id, 0, SETVAL, 1);
    region->semid = sem_id;

    ofstream(INFO_PATH) << shm_id << "\n";

    struct sigaction sa = {};
    sa.sa_handler = stop;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    cout << "server! shmid " << shm_id << ", semid " << sem_id << ", region " << sizeof(Region) << " bytes" << endl;

    string last;
    while (running) {
        string table = "mailboxes: [";
        for (Mailbox &mb : region->mbox)
            table += mb.used ? " " + to_string(mb.pid) : " -";
        table += " ]";
        if (table != last) {
            cout << table << endl;
            last = table;
        }
        usleep(100000);
    }

    shmdt(region);
    shmctl(shm_id, IPC_RMID, nullptr);
    semctl(sem_id, 0, IPC_RMID);
    remove(INFO_PATH);
    cout << "server cleaned up" << endl;
    return 0;
}
