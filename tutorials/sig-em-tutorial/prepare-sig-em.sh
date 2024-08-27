#!/bin/bash

# Issue tokens
cleos -u http://0.0.0.0:8000 push action sysio.token issue '[ "sysio", "1000000000.0000 SYS", "Issue" ]' -p sysio@active

# Create accounts
cleos wallet import --private-key 5JJPh8dLzbz1XYnGoUaCr3LQDj9aWeA52sqGQgqgarYKYpYzsbY
cleos -u http://0.0.0.0:8000 system newaccount sysio settle.wns SYS5bVXRvVGsAfDWCQ1v5m5JRx42AAy6HtskiBpYXxedW8wxGeShb SYS5bVXRvVGsAfDWCQ1v5m5JRx42AAy6HtskiBpYXxedW8wxGeShb --stake-net '1000.0000 SYS' --stake-cpu '1000.0000 SYS'  --buy-ram-kbytes 2048
cleos -u http://0.0.0.0:8000 system newaccount sysio auth.msg SYS5bVXRvVGsAfDWCQ1v5m5JRx42AAy6HtskiBpYXxedW8wxGeShb SYS5bVXRvVGsAfDWCQ1v5m5JRx42AAy6HtskiBpYXxedW8wxGeShb --stake-net '1000.0000 SYS' --stake-cpu '1000.0000 SYS'  --buy-ram-kbytes 2048

# Compile and deploy auth.msg contract
cd contracts/auth.msg
# source ./build.sh
source ./deploy.sh

# Compile and deploy settle.wns contract
cd ../settle.wns
# source ./build.sh
source ./deploy.sh

# Allow auth.msg to add auth.ext permissions on accounts
cleos -u http://0.0.0.0:8000 push action sysio updateauth '{ "account": "auth.msg", "permission": "owner", "parent": "", "auth": { "threshold": 1, 
    "keys": [{
        "key": "PUB_K1_5bVXRvVGsAfDWCQ1v5m5JRx42AAy6HtskiBpYXxedW8wwJWhhS", 
        "weight": 1
    }], "accounts": [{ "permission": { "actor": "auth.msg", "permission": "sysio.code" }, "weight": 1 }], "waits": []}}' -p auth.msg@owner

# Init settle contract
cleos -u http://0.0.0.0:8000 push action settle.wns initcontract '[]' -p settle.wns@active

# HELPERS

# cleos -u http://0.0.0.0:8000 system newaccount sysio ikdfsdhpun.x PUB_EM_8exffEtJ8SsyG5kiwAPwACqLf29ygzbtxoqBFdWTrWB6qxCYGE PUB_EM_8exffEtJ8SsyG5kiwAPwACqLf29ygzbtxoqBFdWTrWB6qxCYGE --stake-net '100.0000 SYS' --stake-cpu '100.0000 SYS'  --buy-ram-kbytes 1024
# cleos -u http://0.0.0.0:8000 system newaccount sysio qhzesdrkcdls PUB_EM_6hx8XEjettHeTg2W2vaw66577g1mwDg2oz4mtkpfx54UqEM99C PUB_EM_6hx8XEjettHeTg2W2vaw66577g1mwDg2oz4mtkpfx54UqEM99C --stake-net '100.0000 SYS' --stake-cpu '100.0000 SYS'  --buy-ram-kbytes 1024
# cleos -u http://0.0.0.0:8000 system newaccount sysio .pbqcja3oeej PUB_EM_6iPWEcH1tgQTjPNCRYfrVBx2MhGv4onomyfybupcUmW5LBHQqt PUB_EM_6iPWEcH1tgQTjPNCRYfrVBx2MhGv4onomyfybupcUmW5LBHQqt --stake-net '100.0000 SYS' --stake-cpu '100.0000 SYS'  --buy-ram-kbytes 1024


# cleos -u http://0.0.0.0:8000 system delegatebw sysio settle.wns "100.0000 SYS" "100.0000 SYS" -p sysio@active
# cleos -u http://0.0.0.0:8000 system delegatebw sysio auth.msg "40.0000 SYS" "40.0000 SYS" -p sysio@active
# cleos -u http://0.0.0.0:8000 system delegatebw sysio ikdfsdhpun.x "100.0000 SYS" "100.0000 SYS" -p sysio@active

# cleos -u http://0.0.0.0:8000 system buyram sysio settle.wns "100.0000 SYS" -p sysio@active
# cleos -u http://0.0.0.0:8000 system buyram sysio auth.msg "100.0000 SYS" -p sysio@active

# cleos -u http://0.0.0.0:8000 system buyram sysio ikdfsdhpun.x "10.0000 SYS" -p sysio@active