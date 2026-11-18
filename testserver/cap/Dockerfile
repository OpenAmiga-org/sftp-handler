# OpenSSH whose SFTP server gives at most CAP bytes per read reply (like
# servers that limit the read size), and that also allows the CBC ciphers,
# for the short-read and cipher tests. Built and started by
# testserver/setup.sh; the cap is read from /srv/cap/cap at every login
# (0 or no file = no cap).
FROM ubuntu:24.04
ARG UID=1000
ARG GID=1000
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        openssh-server python3-minimal && rm -rf /var/lib/apt/lists/*
COPY sshd_config /etc/ssh/sshd_config
COPY sftpcap.py /usr/local/bin/sftpcap.py
RUN set -e; chmod 755 /usr/local/bin/sftpcap.py; mkdir -p /run/sshd; \
    useradd -o -u "$UID" -g "$GID" -M -d /home/test -s /bin/sh test 2>/dev/null || \
    { groupadd -o -g "$GID" sftph; useradd -o -u "$UID" -g "$GID" -M -d /home/test -s /bin/sh test; }; \
    mkdir -p /home/test; ln -s /srv/data /home/test/data; chown -R "$UID:$GID" /home/test; \
    echo 'test:test123' | chpasswd
CMD ["/usr/sbin/sshd", "-D", "-e"]
