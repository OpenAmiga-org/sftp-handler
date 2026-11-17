# OpenSSH with two-factor logins (TOTP codes via the Google Authenticator
# PAM module) for the 2FA tests. Built and started by testserver/setup.sh.
FROM ubuntu:24.04
ARG UID=1000
ARG GID=1000
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        openssh-server libpam-google-authenticator && rm -rf /var/lib/apt/lists/*
COPY sshd_config /etc/ssh/sshd_config
COPY pam_sshd /etc/pam.d/sshd
COPY google_authenticator /tmp/google_authenticator
# the test users share the host user's uid, so all see the files in /srv/data
RUN set -e; mkdir -p /run/sshd; \
    for u in otp keyotp keypw; do \
        useradd -o -u "$UID" -g "$GID" -M -d /home/$u -s /bin/sh $u 2>/dev/null || \
        { groupadd -o -g "$GID" sftph; useradd -o -u "$UID" -g "$GID" -M -d /home/$u -s /bin/sh $u; }; \
        mkdir -p /home/$u; ln -s /srv/data /home/$u/data; \
        cp /tmp/google_authenticator /home/$u/.google_authenticator; \
        chown -R "$UID:$GID" /home/$u; chmod 400 /home/$u/.google_authenticator; \
    done; \
    echo 'otp:otp123' | chpasswd; echo 'keypw:keypw123' | chpasswd; \
    echo 'keyotp:unused-keyotp-9f3' | chpasswd
CMD ["/usr/sbin/sshd", "-D", "-e"]
