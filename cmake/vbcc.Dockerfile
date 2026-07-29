FROM terriblefire78/vbcc:latest
# the upstream image ships its headers as 0600 root-only, which breaks running as a normal user
RUN chmod -R a+rX /opt/vbcc
# gcc-multilib lets test/amigafs-host-test.sh build amigafs.c for a 32-bit host
# against the NDK headers that live in this image
RUN apt-get update -qq \
	&& apt-get install -y -qq --no-install-recommends gcc-multilib \
	&& rm -rf /var/lib/apt/lists/*
