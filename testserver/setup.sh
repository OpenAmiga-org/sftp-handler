#!/bin/bash
# Starts throwaway FTP and SFTP servers in Docker for the tests.
#
#   SFTP  127.0.0.1:2222  user test / test123   (files in testserver/sftp-data)
#   FTP   127.0.0.1:2121  user test / test123   (files in testserver/ftp-data)
#         passive ports 21000-21010
#
# The AmigaOS 4 guest (QEMU user network) reaches them at 10.0.2.2.
# The SFTP host key is kept in testserver/keys so it stays the same.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p sftp-data ftp-data keys
[ -f keys/ssh_host_ed25519_key ] || ssh-keygen -q -t ed25519 -N '' -f keys/ssh_host_ed25519_key
[ -f keys/ssh_host_rsa_key ] || ssh-keygen -q -t rsa -b 3072 -N '' -f keys/ssh_host_rsa_key
# a key pair for the public key login tests
[ -f keys/client_ed25519 ] || ssh-keygen -q -t ed25519 -N '' -f keys/client_ed25519
mkdir -p keys/authorized
cp keys/client_ed25519.pub keys/authorized/authorized_keys

docker rm -f sftph-sftp sftph-ftp >/dev/null 2>&1 || true

docker run -d --name sftph-sftp -p 2222:22 \
	-v "$PWD/keys/ssh_host_ed25519_key:/etc/ssh/ssh_host_ed25519_key:ro" \
	-v "$PWD/keys/ssh_host_rsa_key:/etc/ssh/ssh_host_rsa_key:ro" \
	-v "$PWD/keys/authorized/authorized_keys:/home/test/.ssh/keys/client.pub:ro" \
	-v "$PWD/sftp-data:/home/test/data" \
	atmoz/sftp:alpine "test:test123:$(id -u):$(id -g)" > /dev/null

docker run -d --name sftph-ftp -p 2121:21 -p 21000-21010:21000-21010 \
	-e USERS="test|test123|/ftp/test|$(id -u)" -e ADDRESS=127.0.0.1 \
	-v "$PWD/ftp-data:/ftp/test" \
	delfer/alpine-ftp-server > /dev/null

sleep 2
docker ps --filter name=sftph- --format '{{.Names}}  {{.Status}}  {{.Ports}}'
