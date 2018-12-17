# TO_BUILD: docker build --pull=true .
FROM      centos:7

WORKDIR   /root/highloadcup

# Install tools for HMP build
RUN       yum install -y wget && \
          wget http://dl.fedoraproject.org/pub/epel/epel-release-latest-7.noarch.rpm && \
          rpm -ivh epel-release-latest-7.noarch.rpm && \
          wget https://copr.fedoraproject.org/coprs/g/kdesig/cmake3_EPEL/repo/epel-7/heliocastro-cmake3_EPEL-epel-7.repo -O /etc/yum.repos.d/heliocastro-cmake3_EPEL-epel-7.repo && \
          yum install -y \
            git \
            make \
            cmake3 \
            gcc \
            gcc-c++

RUN yum install -y boost-devel
RUN yum install -y centos-release-scl && \
    yum install -y devtoolset-6-gcc* && \
    scl enable devtoolset-6 bash

ADD     CMakeLists.txt .
ADD     main.cpp .

WORKDIR /root/highloadcup/build

RUN     cmake3 .. -DCMAKE_C_COMPILER=/opt/rh/devtoolset-6/root/usr/bin/gcc -DCMAKE_CXX_COMPILER=/opt/rh/devtoolset-6/root/usr/bin/g++ && make -j4

ENV     HIGHLOADCUP_PORT 80
EXPOSE  ${HIGHLOADCUP_PORT}
CMD     ./highloadcup