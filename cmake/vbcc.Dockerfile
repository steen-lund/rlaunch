FROM terriblefire78/vbcc:latest
# the upstream image ships its headers as 0600 root-only, which breaks running as a normal user
RUN chmod -R a+rX /opt/vbcc
