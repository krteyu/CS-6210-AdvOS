#include "link.h"
using namespace std;

int main(int argc, char *argv[]) {
    int hold = argc > 1 ? atoi(argv[1]) : 1;

    int shm_id;
    ifstream(INFO_PATH) >> shm_id;
    Region *region = (Region *)shmat(shm_id, nullptr, 0);

    lock(region->semid);
    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!region->mbox[i].used) {
            slot = i;
            region->mbox[i].pid = getpid();
            region->mbox[i].used = true;
            break;
        }
    }
    unlock(region->semid);

    if (slot == -1) {
        cout << "pid " << getpid() << ": no free mailbox" << endl;
        shmdt(region);
        return 1;
    }

    cout << "pid " << getpid() << ": got mailbox " << slot << endl;
    sleep(hold);

    region->mbox[slot].used = false;
    shmdt(region);
    cout << "pid " << getpid() << ": released mailbox " << slot << endl;
    return 0;
}
