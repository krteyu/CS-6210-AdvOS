# Project 2 Submission

### 1. General Rule

* Submit through **Gradescope** only. Other types of submission will not be
  accepted, e.g. email.

* There are two separate Gradescope assignments:
    - **Code**: a zip file.
    - **Report**: a PDF. No specific format is required; the ACM template
      (`acmart.cls`), available on Overleaf, is suggested if you'd like one.

* **If you work in a pair**, one of you submits and adds the other as a group
  member on Gradescope.

### 2. Code Structure

The zip must contain only a single top-level directory with your project. The
autograder expects the following at the top level of that directory:

* `Makefile` with two recipes:
    - `make build`: builds the TinyFile service and your sample application.
    - `make clean`: removes all build output.
* `group.txt`: who is submitting, one line per group member (at most 2), in
  exactly this format:

  ```
  <gt username> - <gt username>@gatech.edu - <4210 or 6210>
  ```

  For example: `gburdell3 - gburdell3@gatech.edu - 6210`. The listed emails must
  be exactly the accounts on the Gradescope submission. If `group.txt` is
  missing, malformed, or does not match the Gradescope submitters, the
  submission receives no autograder credit; you will see this as soon as the
  autograder finishes, so you can fix it and resubmit before the deadline.
  **After the deadline (including the late period)**, we check every
  `group.txt` against the course gradebook. A `group.txt` that misstates a
  member or their section is an honor code violation and will be reported as
  one. We do not run this check, or report anything, before then.
* `run-server.sh`: starts the TinyFile service (the autograder makes the
  scripts executable itself, since zip tools often drop that bit; it runs them
  as `./run-server.sh` and `./run-client.sh`). It forwards all of its
  arguments, e.g. `./run-server.sh --n_sms 5 --sms_size 32`.
* `run-client.sh`: runs your sample application. It forwards all of its
  arguments, e.g. `./run-client.sh --file bin/input/Tiny.txt --state SYNC`.

The arguments are the ones listed in the [project description](../README.md#1-project-details).
Everything else (source layout, extra files) is up to you.

Your programs must also follow these rules:

* The client writes its compressed results to the directory given by
  `--output_dir` and prints one `TINYFILE_CST|...` line per request, as described in
  the project description. The service prints `TINYFILE_READY` when it is
  ready for clients, as described there too.
* The service exits cleanly on `SIGINT` or `SIGTERM` and releases all of its IPC
  resources (shared memory, queues, semaphores).

Each script only needs to forward its arguments to your binary. For example,
if your build produces `build/server`:

```sh
#!/bin/bash
exec ./build/server "$@"
```

### Snappy and the size of your zip

The autograder image does **not** have snappy installed. Your zip must contain
the snappy source, and `make build` must build it from there.

Ship only what you need to build:

* **Include** the library's source and its build files.
* **Leave out** snappy's own tests, benchmarks and test data. For the
  google/snappy repository that means the `third_party/` and `testdata/`
  directories; disable its tests and benchmarks in your build (for CMake:
  `SNAPPY_BUILD_TESTS=OFF` and `SNAPPY_BUILD_BENCHMARKS=OFF`).
* **Leave out** the sample inputs in `bin/input`, build output, and any `.git`
  directories or files (a submodule leaves a `.git` file behind). The autograder
  brings its own inputs.

A zip with hundreds of files or many megabytes can be rejected by Gradescope
when you upload it. A submission of a few hundred KB to a couple of MB is normal.

Before you zip your code:

* Verify that a fresh `make build` works on the VM cluster.
* Run `make clean`. Do not pack binaries or other build output.
* Zip the directory, e.g. `zip -r project_2.zip <your-directory>`.

### 3. Late Policy

A penalty of 10% per day will be applied for late submissions, up to 2 days.
For instance, if your score is 90 with a two-day late submission, your final
score would be 90 x 0.8 = 72.

Submissions received more than 2 days late will receive no credit.
