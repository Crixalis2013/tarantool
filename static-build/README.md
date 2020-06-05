# Tarantool static build tooling

These files help to prepare environment for building Tarantool
statically. And builds it.

## Prerequisites

CentOS:

```bash
yum install -y \
    git perl gcc cmake make gcc-c++ libstdc++-static autoconf automake libtool \
    python-msgpack python-yaml python-argparse python-six python-gevent
```


### Usage

```bash
cmake .
make -j
ctest -V
```
