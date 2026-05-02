# magma EPC compose

Magma MME + OAI HSS / SPGWC / SPGWU-tiny / Cassandra / Redis stack
that backs the testbed eNB. Adapted from
[openair-epc-fed magma-mme-demo](https://github.com/OPENAIRINTERFACE/openair-epc-fed/tree/main/docker-compose/magma-mme-demo)
with one fix: `DEFAULT_DNS_IPV4_ADDRESS = 8.8.8.8` (upstream pointed
at a campus resolver unreachable from a UE attached to your testbed).

## Environment

- Docker ≥ 20, docker-compose ≥ 2

Pull + retag images:

```bash
docker pull rdefosseoai/magma-mme:latest               && \
docker pull oaisoftwarealliance/oai-hss:latest         && \
docker pull oaisoftwarealliance/oai-spgwc:latest       && \
docker pull oaisoftwarealliance/oai-spgwu-tiny:latest  && \
docker pull oaisoftwarealliance/trf-gen-cn5g:jammy     && \
docker pull cassandra:2.1 && docker pull redis:6.0.5

docker tag rdefosseoai/magma-mme:latest              magma-mme:master
docker tag oaisoftwarealliance/oai-hss:latest        oai-hss:production
docker tag oaisoftwarealliance/oai-spgwc:latest      oai-spgwc:production
docker tag oaisoftwarealliance/oai-spgwu-tiny:latest oai-spgwu-tiny:production
docker tag oaisoftwarealliance/trf-gen-cn5g:jammy    trf-gen:production
```

## Run

```bash
sudo docker-compose up -d cassandra db_init
sudo docker logs demo-db-init -f         # wait for "OK"
sudo docker rm -f demo-db-init
sudo docker-compose up -d                # rest of EPC
```

All containers should report `healthy` after ~30 s.

## SIM keys

`docker-compose.yml` HSS env:

```yaml
LTE_K:  fec86ba6eb707ed08905757b1bb44b8f
OP_KEY: 1006020f0a478bf6b699f15c062e42b3
FIRST_IMSI: "001010000000007"
APN1: oai.ipv4
```

Burn into your SIM (we use sysmocom programmable SIM + `pySim`) or
change them to match an existing SIM and restart `oai_hss`.
PLMN = `001/01`.

## Tear down

```bash
sudo docker-compose down       # keep cassandra volume
sudo docker-compose down -v    # full reset
```
