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
#     from the mounted project, which is the checkout being edited);
#   * agents — tools/agents/kiln_agents' serve.py on the CrewAI env
#     (nix/agent-python.nix's crewPython — the same env the agent gates build,
#     so what the gates proved is what the container runs) plus git, nix and
#     the Claude Code CLI, which ClaudeWorker shells out to. The agents
#     container builds through the host daemon exactly like the studio one
#     (ClaudeWorker's allowlist includes `nix build:*`), it never phones a
#     model except Anthropic and Ollama, and its keys arrive as FILES from
#     sops-nix, mounted by the module — never baked here.
{ pkgs, lib, toolsSrc, agentPython, claude }:

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

  python = pkgs.python3Minimal;

  server = pkgs.writeShellScriptBin "kiln-studio-server" ''
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
    paths = [
      server python etc
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
  studio = studio // { inherit runtime studioTools; };
  agents = agents // { runtime = agentsRuntime; tools = agentsTools; };
}
