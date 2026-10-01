# Data Flow Overview
## Initialization
1. Server spins up first, generating an information file- `tmp/ring_info.txt`, which stores the `shmid` for each clients pair of ringbuffers (incoming and outgoing). This file also contains the shmid for the shared memory segments.
1. Client spins up, reads `tmp/ring_info.txt`, maps the ring buffers to its memory and begins normal operation
## Data Flow
1. Client sends a request for a memory segment (`SEG_REQ`) to the server. 
1. The server chooses a memory segment and allots it to a given client by sending a `SEG_GRANT` message
1. Client moves as much data as possible into the segment and sends a 'DATA_READY' message to the server.
    1. If the file manages to fit entirely inside the segment, the message is of type `SNGL`
    2. If not, the first message is of type `INIT`. The next message is of type `CONT` if its isn't the final chunk. The final chunk is of type `FIN`.
    3. The client should only keep in-flight a certain max number of requests for segments. The server keeps track of how many segments have been requested and grant it according to its scheduling policy. Segments are granted with `SEG_GRANT` messages and are revoked when the segments are used by a `DATA_READY` message.
    4. The server stores the chunks into local memory, once the whole file is collected, it compresses it.
1. The server returns the compressed file through the client's incoming (Server-> Client) ringbuffer. 
1. The server sends `DATA_READY` messages to the client, wiht chunks of the compressed file. The server can buffer as many messages as there are free shared segments. (TO-DO: Consider failure conditions and revocation)
1. If the request was `ASYNC`, the server sends a SIGUSR interrupt to the client. If `SYNC`, the client will wait on its incoming ringbuffer's semaphore.

Note: We can use the `SEG_GRANT` to throttle clients that are rate-limited, we can play around with the policy here
## Cleanup
1. When the server receives `SIGTERM` or `SIGINT`, it duplicates that signal to all its clients
    1. When a client receives a `SIGTERM` or `SIGINT` it detaches from all its shared memories, and exits gracefully (Note- It could also enter a holding pattern waiting for a new info file, indicating a new server)
1. It deletes the server info file
1. It detaches and releases all shared memory resources
1. It exits gracefully
