#!/usr/bin/env python3
import asyncio, sys
from aioesphomeapi import APIClient, ButtonInfo
async def main():
    cli = APIClient(sys.argv[1], 6053, None)
    await cli.connect(login=True)
    ents, _ = await cli.list_entities_services()
    for e in ents:
        if isinstance(e, ButtonInfo) and "restart" in e.object_id.lower() and "safe" not in e.object_id.lower():
            print("pressing", e.object_id, e.key)
            cli.button_command(key=e.key)
            await asyncio.sleep(1)
            break
    await cli.disconnect()
asyncio.run(main())
