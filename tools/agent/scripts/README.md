# Workspace scripts

The agent tooling expects a workspace folder laid out like this:

```
<workspace>\
  ariane\                  this repository (built: ariane\bin\win-amd64-d3d9\Release\ariane.exe)
  agent-data\
    token.txt              agent bridge token (any random string)
    assets.sqlite3         asset index (arianectl assets index)
    state\                 checkpoints, prefabs, suppression journal
    discovery\
    samp_layer_agent.ipl   scene file the agent layer is bound to
  .venv-agent\             python -m venv .venv-agent; pip install -e "ariane[mcp]"
  run_ariane_samp.bat      from run_ariane_samp.bat.template
  arianectl.cmd            from arianectl.cmd.template
  ariane-mcp.cmd           from ariane-mcp.cmd.template (point your MCP host at it)
```

Copy the `*.template` files to the workspace root, drop the `.template`
suffix and set the game folder (`ARIANE_GAME_DIR`) and port in each.

* `restart_ariane.ps1 [-Checkpoint name] [-NoSave] [-Workspace dir] [-Port n]` -
  checkpoint, restart Ariane, rebind the agent scene, restore.
* `show_ariane.ps1` - un-minimise Ariane (a minimised window captures as 0x0).
