Scribe: The record-replay mechanism
=====================================

Abstract
--------

Scribe is a low-overhead multi-threaded application record-replay mechanism.

Scribe introduces new lightweight operating system mechanisms, rendezvous and
sync points, to efficiently record nondeterministic interactions such as
related system calls, signals, and shared memory accesses.  Rendezvous points
make a partial ordering of execution based on system call dependencies
sufficient for replay, avoiding the recording overhead of maintaining an exact
execution ordering.  Sync points convert asynchronous interactions that can
occur at arbitrary times into synchronous events that are much easier to
record and replay.

For more details read [this blog post](http://velvetpulse.com/2012/11/27/scribe-the-deterministic-transparent-record-replay-engine/).
It has a screencast showing some of Scribe features.

Project Organisation
---------------------

The Scribe project is divided in four different ones:

- [The Linux Kernel](/nviennot/linux-2.6-scribe)
- [The Userspace C Library](/nviennot/libscribe)
- [The Python Library](/nviennot/py-scribe)
- [The Tests](/nviennot/tests-scribe) (broken)

Installing Scribe
---------------------

### Prerequisites:

- GCC and its friends
- CMake
- Python (**version 2.6**)
- Cython

### Instructions:

1. Install the kernel

        git clone git://github.com/nviennot/linux-2.6-scribe.git
        cd linux-2.6-scribe
        make menuconfig
        make
        make install

2. Install the C library

        git clone git://github.com/nviennot/libscribe.git
        cd libscribe
        cd build
        cmake ..
        make install

3. Install the python library and userspace tools

        git clone git://github.com/nviennot/py-scribe.git
        cd py-scribe
        ./setup install

4. (Optional) Install the test suite

        git clone git://github.com/nviennot/tests-scribe.git

Using Scribe
-------------

py-scribe provides three scripts: record, replay, profiler.

### 1. Record an application

The __record__ command line tool allows the user to record an execution.

The verbosity level of the recorded log file can be provided. It allows the
user to record only the bare minimum to guarantee a deterministic replay
(highest performance), or to record the execution with debugging information so
the log file can be easily interpreted by getting a execution trace similar to
strace.

By sending a `SIGUSR1` signal to the recording tool, Scribe detaches itself
from the application while it continues running.
A `SIGUSR2` signal bookmarks an execution point in time. The user can then
replay the application up to that point and the application state will be
guaranteed to be exactly the same as during the recording.

Example:

        # record date
        Mon Aug  8 04:18:33 EDT 2011
        # ls -lh date.log
        -rw-r--r-- 1 root root 4.2K Aug  8 04:18 date.log

### 2. Replay an execution from a log file

The __replay__ tool allows the user to replay a previously recorded execution.

The user can provide the backtrace size in case the replay fails and diverge
(for instance, the system got out of memory and the replay cannot continue).

A `SIGUSR1` signal can be sent to detach Scribe at any point in time and let the
application continue a normal execution.

A bookmark id can be given as well to let the application _go live_ at a
specific point in time.

Example:

        # replay date.log
        Mon Aug  8 04:18:33 EDT 2011

### 3. Look at the recorded log file in a human readable format

The __profiler__ tool allows the user to display the recorded log file in a human readable format.

Example:

        # profiler date.log | grep Mon -B3
        [02] write() = 29
        [02]   resource lock, type = files_struct, serial = 31
        [02]     resource lock, type = file, serial = 1, desc = /dev/pts/0
        [02]       data: size = 29, "Mon Aug  8 04:18:33 EDT 2011\n"

The provided command line tools use the Scribe Python library internally.
The user can use the libraries to achieve a lot more by building its own logic
around the Scribe API.

Detailed documentation
-----------------------

- For the kernel implementation details, read the
[scribe kernel documentation](/nviennot/linux-2.6-scribe/blob/master/Documentation/scribe.md) (OUTDATED).
