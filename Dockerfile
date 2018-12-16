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

ADD     CMakeLists.txt .
ADD     main.cpp .

WORKDIR /root/highloadcup/build

RUN     cmake3 .. && make -j4

EXPOSE 80

CMD     ./highloadcup