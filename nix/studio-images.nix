# SPDX-License-Identifier: MIT
#
# nix/studio-images.nix — the container images Kiln Studio runs.
#
# dockerTools, not nixos-generators (packages.dev-image): the studio is one
# process, and a NixOS init inside a rootless container fights the user
# namespace for nothing. The image has no Nix store of its own worth the name.
# nix/studio-module.nix mounts the host's /nix/store read-only over it and
# hands the host daemon's socket in, so a build the studio starts is a build on
# the host — sandboxed by the host's nix.conf, bounded by its cores/max-jobs
# rather than by this container's cgroup — and its output is visible here the
# moment it exists.
#
# That is also why `runtime` is exposed. The image tarball is compressed, so it
# carries no store references, and nothing roots the paths inside it; once the
# host's store is mounted over the image's own copy, the collector would be
# free to delete the very interpreter the container runs. The module adds
# `runtime` to the system closure, which roots all of it. Both images work
# the same way; both carry their own.
#
# Two images, one shape:
#
#   * studio — the web app and its job runner (python3Minimal; validators run
#     from the mounted project, which is the checkout being edited), and a
#     second MODE of the same entrypoint: `kiln-studio-server mesh` runs
#     HydraMesh's mesh_mcp (streamable HTTP, :8765) — nix/studio-module.nix's
#     compose file runs it as a third service off this image;
#   * agents — tools/agents/kiln_agents' serve.py on the CrewAI env
#     (nix/agent-python.nix's crewPython — the same env the agent gates build,
#     so what the gates proved is what the container runs) plus git, nix and
#     the Claude Code CLI, which ClaudeWorker shells out to. The agents
#     container builds through the host daemon exactly like the studio one
#     (ClaudeWorker's allowlist includes `nix build:*`), it never phones a
#     model except Anthropic and Ollama, and its keys arrive as FILES from
#     sops-nix, mounted by the module — never baked here.
{ pkgs, lib, toolsSrc, agentPython, meshSrc, claude }:

let
  # The studio as deployed: its server and page, and the editors it serves.
  # Validators run from the mounted project, which is the checkout being edited.
  studioTools = lib.fileset.toSource {
    root = toolsSrc;
    fileset = lib.fileset.unions [
      (toolsSrc + "/studio")
      (toolsSrc + "/mapmaker/index.html")
      (toolsSrc + "/mapmaker/src")
      (toolsSrc + "/mapmaker/vendor")
      (toolsSrc + "/poser/index.html")
      (toolsSrc + "/poser/src")
      (toolsSrc + "/webcommon")
    ];
  };

  # The agents as deployed: the flow runtime and its tools. Baked rather than
  # read from the mounted project for the same reason the studio server is: a
  # service should run the reviewed code, not whatever a worktree's branch
  # currently says. Task WORK happens in the mounted project's worktrees.
  agentsTools = lib.fileset.toSource {
    root = toolsSrc;
    fileset = toolsSrc + "/agents";
  };

  # HydraMesh's mesh MCP server, as deployed: SOURCE ONLY from the pinned
  # hydramesh input — nothing here builds HydraMesh's own packages. mesh_mcp
  # is pure Python over the `mcp` package, so it runs on browserPython
  # (nix/agent-python.nix's mcp + websockets env), not the CrewAI one.
  #
  # A copy, not lib.fileset: a `flake = false` input arrives here as a STRING
  # with store context, which lib.fileset refuses (and `/. + str` cannot
  # convert), while a derivation interpolates it fine. The subset is the
  # import closure, traced by hand against the fetched rev: mesh_mcp.py
  # pulls a2a_endpoint, dcf_node and dcf_text (matrix-bridge — its own
  # dcf_node.py, the DcfTextNode the MCP server is built on, NOT python's
  # ProtoMessage-CLI of the same name), and those pull superpack /
  # wirelab_core / textlab_core / meshlab_core (python/MCP). The layout is
  # preserved because matrix-bridge's own sys.path insertions are relative
  # (`../python/MCP`), so the copy must keep both directories side by side.
  meshTools = pkgs.runCommand "kiln-mesh-tools" { } ''
    mkdir -p $out/matrix-bridge $out/python
    cp ${meshSrc}/matrix-bridge/mesh_mcp.py \
       ${meshSrc}/matrix-bridge/a2a_endpoint.py \
       ${meshSrc}/matrix-bridge/dcf_node.py \
       ${meshSrc}/matrix-bridge/dcf_text.py \
       $out/matrix-bridge/
    cp -r ${meshSrc}/python/MCP $out/python/
  '';

  # Rev 237d201's `_run_remote` ends with an unguarded
  # `mcp.run_streamable_http(port=port)` — only its SSE arm is version-guarded —
  # while browserPython's `mcp` spells the same thing
  # `run(transport="streamable-http")` and has no such method.  The adapter adds
  # the missing NAME rather than patching the pinned source, and is a no-op the
  # day a matching `mcp` lands in the pin.  Found by studio-module's VM check:
  # the mesh container crashed on boot and compose's --abort-on-container-exit
  # escalated that one exit into the whole project flapping.
  # writeText, not writers.writePython3: the latter flake8-checks the result
  # and the interpolated store path below cannot fit in 79 columns.  The body
  # is the one smoked live against browserPython + meshTools (see the mesh
  # commit): /mcp answers 406 on a bare GET, /nope 404.
  meshAdapter = pkgs.writeText "kiln-mesh-adapter.py" ''
    import runpy
    import sys

    from mcp.server.fastmcp import FastMCP

    if not hasattr(FastMCP, "run_streamable_http"):
        def _run_streamable_http(self, port=None):
            if port is not None:
                try:
                    self.settings.port = port
                except Exception:
                    pass
            return self.run(transport="streamable-http")

        FastMCP.run_streamable_http = _run_streamable_http

    # argv[1:] is already mesh_mcp's own ("http", maybe a port); only
    # argv[0] must name the script the __main__ block came from.
    sys.argv[0] = "${meshTools}/matrix-bridge/mesh_mcp.py"
    runpy.run_path(sys.argv[0], run_name="__main__")
  '';

  python = pkgs.python3Minimal;

  # One entrypoint, two modes: the studio's own server, and (argv[1] = "mesh")
  # HydraMesh's mesh_mcp in streamable-HTTP mode — the third compose service
  # runs the same image with `command: ["mesh"]`, so there is no second image
  # to build, load or root. Bind host and port come from the environment
  # (DCF_MCP_HTTP_HOST/DCF_MCP_HTTP_PORT), set by nix/studio-module.nix.
  server = pkgs.writeShellScriptBin "kiln-studio-server" ''
    if [ "''${1:-}" = "mesh" ]; then
      shift
      export PYTHONPATH="${meshTools}/python:${meshTools}/python/MCP:${meshTools}/matrix-bridge''${PYTHONPATH:+:$PYTHONPATH}"
      exec ${agentPython.browserPython}/bin/python3 ${meshAdapter} http "$@"
    fi
    exec ${python}/bin/python3 ${studioTools}/studio/server.py "$@"
  '';

  agentsServer = pkgs.writeShellScriptBin "kiln-agents-server" ''
    # models.py reads the key FILE; the Claude Code CLI wants the plain env
    # var, so promote it once, here, rather than in every caller.
    if [ -n "''${ANTHROPIC_API_KEY_FILE:-}" ] && [ -f "''${ANTHROPIC_API_KEY_FILE}" ]; then
      export ANTHROPIC_API_KEY="$(cat "''${ANTHROPIC_API_KEY_FILE}")"
    fi
    export PYTHONPATH="${agentsTools}/agents''${PYTHONPATH:+:$PYTHONPATH}"
    exec ${agentPython.crewPython}/bin/python3 -m kiln_agents.serve
  '';

  etc = pkgs.runCommand "kiln-studio-etc" { } ''
    mkdir -p $out/etc/nix
    printf 'root:x:0:0:root:/root:/bin/bash\n' > $out/etc/passwd
    printf 'root:x:0:\n' > $out/etc/group
    printf 'experimental-features = nix-command flakes\n' > $out/etc/nix/nix.conf
    # A git identity at the SYSTEM level: neither container has a writable HOME
    # before /root's tmpfs is mounted, and the agents' Git tool (and the
    # studio's worktree lifecycle) would refuse every commit with "tell me
    # who you are" — the first task in the container would fail at its commit.
    printf '[user]\n\tname = kiln-agents\n\temail = agents@kiln.invalid\n' > $out/etc/gitconfig
  '';

  runtime = pkgs.buildEnv {
    name = "kiln-studio-runtime";
    # Two python environments on purpose (3.14 minimal for the studio server,
    # browserPython 3.13 for mesh mode), so their bin/ overlaps (pydoc and
    # friends). Both entrypoints call their interpreter by ABSOLUTE path and
    # the paths order keeps /bin/python3 the studio's 3.14, so the collisions
    # are cosmetic — but understood ones, not surprises.
    ignoreCollisions = true;
    paths = [
      server python etc
      # mesh mode's interpreter: mesh_mcp runs on the mcp+websockets env, not
      # python3Minimal. In `paths` so the module's rooting of `runtime` (see
      # header) keeps it alive once the host's store is mounted over.
      agentPython.browserPython
      pkgs.bashInteractive pkgs.coreutils pkgs.gnugrep pkgs.gnused pkgs.findutils
      pkgs.git pkgs.nix pkgs.cacert
    ];
    pathsToLink = [ "/bin" "/etc" ];
  };

  studio = pkgs.dockerTools.buildLayeredImage {
    name = "kiln-studio";
    contents = [ runtime ];
    extraCommands = ''
      mkdir -p tmp root nix/var/nix/daemon-socket
      chmod 1777 tmp
    '';
    config = {
      Entrypoint = [ "/bin/kiln-studio-server" ];
      Env = [
        "PATH=/bin"
        "HOME=/root"
        "NIX_REMOTE=daemon"
        "SSL_CERT_FILE=/etc/ssl/certs/ca-bundle.crt"
        "NIX_SSL_CERT_FILE=/etc/ssl/certs/ca-bundle.crt"
      ];
      ExposedPorts."8420/tcp" = { };
    };
  };

  agentsRuntime = pkgs.buildEnv {
    name = "kiln-agents-runtime";
    paths = [
      agentsServer agentPython.crewPython claude etc
      pkgs.bashInteractive pkgs.coreutils pkgs.gnugrep pkgs.gnused pkgs.findutils
      pkgs.git pkgs.nix pkgs.cacert
    ];
    pathsToLink = [ "/bin" "/etc" ];
  };

  agents = pkgs.dockerTools.buildLayeredImage {
    name = "kiln-agents";
    contents = [ agentsRuntime ];
    extraCommands = ''
      mkdir -p tmp root nix/var/nix/daemon-socket
      chmod 1777 tmp
    '';
    config = {
      Entrypoint = [ "/bin/kiln-agents-server" ];
      Env = [
        "PATH=/bin"
        "HOME=/root"
        "NIX_REMOTE=daemon"
        "SSL_CERT_FILE=/etc/ssl/certs/ca-bundle.crt"
        "NIX_SSL_CERT_FILE=/etc/ssl/certs/ca-bundle.crt"
      ];
      ExposedPorts."8600/tcp" = { };
    };
  };
in
{
  # `runtime` rides along on each image so nix/studio-module.nix can root the
  # store paths the tarball itself does not reference (see the header).
  # `meshTools` is the HydraMesh source that the mesh mode runs — exported for
  # the same reason: nothing else roots the input once the image is a tarball.
  studio = studio // { inherit runtime studioTools meshTools; };
  agents = agents // { runtime = agentsRuntime; tools = agentsTools; };
}
