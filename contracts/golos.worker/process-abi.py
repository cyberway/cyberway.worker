#!/usr/bin/env python3
import sys
import json
import logging
import re

if __name__ == "__main__":
    logging.basicConfig(level=logging.DEBUG)

    abi = json.load(sys.stdin)
    # add application domain name
    for action in abi["actions"]: # add app_domain argument to all actions
        for struct in abi["structs"]:
            if struct["name"] != action["name"]:
                continue

            struct["fields"].insert(0, {
                "name": "app_domain",
                "type": "name"
            })

    # # remove unused types
    # abi["structs"] = list(filter(lambda x: x is not None,
    #     [struct if struct["name"] not in ["block_timestamp"] else None for struct in abi["structs"]]))

    # # patch existent types
    # for t in abi["types"]:
    #     if t["new_type_name"] == "symbol_name": # patch for eosjs
    #         t["type"] = "symbol_code"

    # # add new types
    # for type_name, type_base in {"account_name": "name", "block_timestamp": "uint32"}.items():
    #     abi["types"].append({
    #         "new_type_name": type_name,
    #         "type": type_base
    #     })

    json.dump(abi, sys.stdout, indent=4, separators=(',', ': '))
