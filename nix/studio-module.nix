# SPDX-License-Identifier: MIT
#
# nix/studio-module.nix — nixosModules.kiln-studio: Kiln Studio on a NixOS
# workstation, in a rootless container, shared with a small team over a tailnet.
#
# Written to Oligarchy NixOS's rules, and plain enough for any NixOS:
#
#   * Rootless docker only. A user unit runs `docker compose` against the
#     user's own daemon — no virtualisation.oci-containers, nothing rootful
#     (Oligarchy removed the docker group as root-equivalent). Rootful docker
#     still works on a host that has it.
#   * Reachable only through the tailnet. The container's port is published on
#     127.0.0.1 alone; `tailscale serve` proxies the tailnet to it, and the
#     firewall opens the serve port on the tailscale interface only — never
#     trustedInterfaces, never globally.
#   * Identity from Tailscale. serve adds Tailscale-User-Login, which the studio
#     believes only for allowedLogins and only from the proxy's address. Under
#     rootless docker a host-loopback client reaches the container from the
#     compose network's gateway (measured: never 127.0.0.1), so the network's
#     subnet and gateway are fixed here and the gateway is what the studio
#     trusts. That is the same trust as 127.0.0.1 on a bare host: any local
#     process that can reach the published port could send the header.
#   * No Nix store of its own. The host's store read-only plus the daemon
#     socket (nix/studio-images.nix explains the consequences).
#   * Optional agents container (custom.kilnStudio.agents.enable): the CrewAI
#     runtime plus the Claude Code worker, same compose network, same project
#     checkout, model keys mounted as files — never in the image, never in the
#     store. It reaches Anthropic and Ollama only, and only when the agents
#     are enabled does the module ask the egress firewall for those names.
#   * HydraMesh container (custom.kilnStudio.hydramesh.enable, on by default):
#     the mesh's MCP server (mesh_mcp, streamable HTTP) off the same studio
#     image, compose-network only, given to the agents as KILN_MESH_URL.
#     A plaintext small-message relay — safe inside the docker network the
#     same way HydraMesh is safe inside WireGuard.
#   * Options under custom.kilnStudio, off by default. No `follows` is forced on
#     the importing flake; checks.studio-module boots it on nixos-25.11.
{ kiln }:

{ config, lib, pkgs, options, ... }:

let
  inherit (lib) mkOption mkIf mkMerge types optional optionalAttrs optionalString concatMap;

  cfg = config.custom.kilnStudio;
  system = pkgs.stdenv.hostPlatform.system;

  image = kiln.packages.${system}.studio-image
    or (throw "custom.kilnStudio: the Kiln flake has no studio-image for ${system}");
  imageRef = "${image.imageName}:${image.imageTag}";

  # The agents container (CrewAI runtime + Claude Code worker). Enabled with
  # custom.kilnStudio.agents.enable; needs its image from the same flake.
  agentsImage = kiln.packages.${system}.agents-image or null;
  agentsImageRef =
    if agentsImage != null then "${agentsImage.imageName}:${agentsImage.imageTag}" else null;

  # Egress the agents container genuinely needs: the two model providers
  # (Anthropic for the Claude roles and the Claude Code worker, Ollama cloud
  # for the content/validator roles), and nothing else — telemetry is off in
  # kiln_agents/__init__.py, so these are the only names it should ever dial.
  agentEgress = [ "api.anthropic.com" "claude.ai" "statsig.anthropic.com" "ollama.com" ];

  rootless = config.virtualisation.docker.rootless.enable;
  docker = if rootless then config.virtualisation.docker.rootless.package else config.virtualisation.docker.package;

  tsIface = config.services.tailscale.interfaceName or "tailscale0";
  scheme = if cfg.tailscaleServe.https then "https" else "http";
  servePort = if cfg.tailscaleServe.port != null then cfg.tailscaleServe.port
              else if cfg.tailscaleServe.https then 443 else 80;

  # What a container can and cannot do, so the hub disables what is not there
  # instead of offering an emulator window nobody can see.
  caps = pkgs.writeText "kiln-studio-caps.json" (builtins.toJSON {
    container = true; toolchain = true; tailscale = cfg.tailscaleServe.enable;
    ares = false; hyprland = false; grim = false; blender = false; chromium = false; sc64 = false; ed64 = false;
  });

  serverArgs = [
    "--repo" cfg.projectDir "--host" "0.0.0.0" "--port" "8420"
    "--trusted-proxy" cfg.network.gateway
    "--max-jobs" (toString cfg.build.maxConcurrentJobs)
    "--caps-file" "${caps}"
  ] ++ concatMap (h: [ "--allow-host" h ]) cfg.allowedHosts
    ++ concatMap (l: [ "--tailscale-login" l ]) cfg.allowedLogins;

  # Mounted read-only into the agents container at fixed points; sops-nix
  # paths (or any file readable by cfg.user). Never an image Env value.
  secretMounts = optional (cfg.secrets.anthropicKeyFile != null)
    "${cfg.secrets.anthropicKeyFile}:/run/secrets/anthropic-key:ro"
  ++ optional (cfg.secrets.ollamaKeyFile != null)
    "${cfg.secrets.ollamaKeyFile}:/run/secrets/ollama-key:ro";

  # JSON is YAML; compose reads either.
  composeFile = pkgs.writeText "kiln-studio-compose.json" (builtins.toJSON {
    name = "kiln-studio";
    networks.kiln.ipam.config = [{ inherit (cfg.network) subnet gateway; }];
    services = {
      studio = {
        image = imageRef;
        container_name = "kiln-studio";
        init = true;
        restart = "no";                     # systemd owns restarts
        command = serverArgs;
        working_dir = cfg.projectDir;
        ports = [ "127.0.0.1:${toString cfg.port}:8420" ];
        networks = [ "kiln" ];
        cpus = cfg.studio.cpus;
        mem_limit = cfg.studio.memory;
        pids_limit = 4096;
        read_only = true;
        tmpfs = [ "/tmp:size=1g" "/root:size=512m" ];
        cap_drop = [ "ALL" ];
        security_opt = [ "no-new-privileges:true" ];
        volumes = [
          "${cfg.projectDir}:${cfg.projectDir}"
          "/nix/store:/nix/store:ro"
          "/nix/var/nix/daemon-socket:/nix/var/nix/daemon-socket:ro"
        ];
        # The studio hands tasks to the agents service over the compose
        # network (KILN_AGENTS_URL, which studio/agents.py reads) — the
        # port is never published: nothing outside the compose network can
        # submit a task.
        environment = optionalAttrs cfg.agents.enable {
          KILN_AGENTS_URL = "http://kiln-agents:8600";
        };
      };
    } // optionalAttrs cfg.agents.enable {
      agents = {
        image = agentsImageRef;
        container_name = "kiln-agents";
        init = true;
        restart = "no";
        working_dir = cfg.projectDir;
        networks = [ "kiln" ];
        cpus = cfg.agents.cpus;
        mem_limit = cfg.agents.memory;
        pids_limit = 4096;
        read_only = true;
        tmpfs = [ "/tmp:size=1g" "/root:size=1g" ];
        cap_drop = [ "ALL" ];
        security_opt = [ "no-new-privileges:true" ];
        volumes = [
          "${cfg.projectDir}:${cfg.projectDir}"
          "/nix/store:/nix/store:ro"
          "/nix/var/nix/daemon-socket:/nix/var/nix/daemon-socket:ro"
        ] ++ secretMounts;
        environment = {
          KILN_REPO = cfg.projectDir;
          # The studio, as addressed from inside the compose network.
          KILN_STUDIO_URL = "http://kiln-studio:8420";
          ANTHROPIC_API_KEY_FILE = "/run/secrets/anthropic-key";
          OLLAMA_API_KEY_FILE = "/run/secrets/ollama-key";
        } // optionalAttrs cfg.hydramesh.enable {
          # The mesh MCP, as the agents' CrewAI flow dials it (flow.py attaches
          # it to every agent as an MCP server). FastMCP's streamable-HTTP
          # transport mounts at /mcp.
          KILN_MESH_URL = "http://kiln-mesh:${toString cfg.hydramesh.port}/mcp";
        };
      };
    } // optionalAttrs cfg.hydramesh.enable {
      # HydraMesh's mesh_mcp — the third service, off the SAME studio image
      # (`command: ["mesh"]` flips the entrypoint's mode). Compose-network
      # only, like the agents service: nothing outside can reach it, and it
      # needs no egress, no project checkout and no daemon socket — it is a
      # plaintext small-message relay, safe inside the docker network the
      # same way the tailnet's WireGuard makes it safe across hosts.
      mesh = {
        image = imageRef;
        container_name = "kiln-mesh";
        init = true;
        restart = "no";
        command = [ "mesh" ];
        networks = [ "kiln" ];
        cpus = "1";
        mem_limit = "512m";
        pids_limit = 1024;
        read_only = true;
        tmpfs = [ "/tmp:size=128m" "/root:size=128m" ];
        cap_drop = [ "ALL" ];
        security_opt = [ "no-new-privileges:true" ];
        volumes = [ "/nix/store:/nix/store:ro" ];
        environment = {
          # mesh_mcp binds loopback by default (HydraMesh's VPN-only rule);
          # inside the compose network the whole interface IS the VPN, so
          # binding wide here is the same rule applied one network in.
          DCF_MCP_HTTP_HOST = "0.0.0.0";
          DCF_MCP_HTTP_PORT = toString cfg.hydramesh.port;
        } // cfg.hydramesh.environment;
      };
    };
  });

  # NB the whole body is ONE argument to writeShellScript. Split it as
  # writeShellScript ... ''...'' + optionalString ... and Nix concatenates
  # onto the DERIVATION's outPath, not the script body — the unit then
  # references `/nix/store/...-kiln-studio-upif ! docker image inspect ...`
  # and systemd spawns a store path that was never built (203/EXEC), which is
  # exactly what checks.studio-module's first run caught.
  up = pkgs.writeShellScript "kiln-studio-up" (''
    set -euo pipefail
    if ! ${docker}/bin/docker image inspect ${imageRef} >/dev/null 2>&1; then
      echo "kiln-studio: loading ${imageRef}"
      ${docker}/bin/docker load -i ${image}
    fi
  '' + optionalString (cfg.agents.enable) ''
    if ! ${docker}/bin/docker image inspect ${agentsImageRef} >/dev/null 2>&1; then
      echo "kiln-studio: loading ${agentsImageRef}"
      ${docker}/bin/docker load -i ${agentsImage}
    fi
  '' + ''
    exec ${pkgs.docker-compose}/bin/docker-compose -f ${composeFile} up --remove-orphans --abort-on-container-exit
  '');
  down = pkgs.writeShellScript "kiln-studio-down" ''
    exec ${pkgs.docker-compose}/bin/docker-compose -f ${composeFile} down --remove-orphans
  '';
in
{
  options.custom.kilnStudio = {
    enable = lib.mkEnableOption "Kiln Studio in a rootless container, shared over the tailnet";

    user = mkOption {
      type = types.nullOr types.str;
      default = if options.custom ? user && options.custom.user ? name then config.custom.user.name else null;
      defaultText = lib.literalExpression "config.custom.user.name on Oligarchy, otherwise null";
      description = "The account whose rootless docker runs the studio. Files the studio writes are owned by it.";
    };

    projectDir = mkOption {
      type = types.str;
      example = "/home/asher/Documents/M64";
      description = ''
        The game checkout the studio works on. Mounted read-write at the same
        path inside the container, so paths in logs, git worktrees and error
        messages mean the same thing on both sides.
      '';
    };

    port = mkOption {
      type = types.port;
      default = 8420;
      description = "Loopback port the container is published on; `tailscale serve` proxies to it.";
    };

    allowedLogins = mkOption {
      type = types.listOf types.str;
      default = [ ];
      example = [ "you@example.com" "teammate@example.com" ];
      description = "Tailscale logins that may use the studio. Everyone else on the tailnet gets 401.";
    };

    allowedHosts = mkOption {
      type = types.listOf types.str;
      default = [ ];
      example = [ "nixos.taile2de2b.ts.net" ];
      description = "Host names the studio answers to besides localhost — the machine's MagicDNS name.";
    };

    tailscaleServe = {
      enable = mkOption {
        type = types.bool;
        default = true;
        description = "Run `tailscale serve` in front of the studio and open its port on the tailscale interface.";
      };
      https = mkOption {
        type = types.bool;
        default = false;
        description = ''
          Serve HTTPS with the tailnet's certificate. Needs HTTPS Certificates
          enabled in the tailnet admin console, which a module cannot do; until
          then plain HTTP inside the WireGuard tunnel still carries identity.
        '';
      };
      port = mkOption {
        type = types.nullOr types.port;
        default = null;
        description = "Tailnet port to serve on; 443 for HTTPS, 80 otherwise.";
      };
    };

    studio = {
      cpus = mkOption { type = types.str; default = "12"; description = "CPU limit for the studio container."; };
      memory = mkOption { type = types.str; default = "16g"; description = "Memory limit for the studio container."; };
    };

    agents = {
      enable = mkOption {
        type = types.bool;
        default = false;
        description = ''
          Run the agents container next to the studio: kiln_agents' serve.py
          (CrewAI flows, one git worktree per task) plus the Claude Code
          worker, on the same compose network and the same project checkout.
          The studio's Agents panel submits tasks to it over that network.
        '';
      };
      cpus = mkOption { type = types.str; default = "4"; description = "CPU limit for the agents container."; };
      memory = mkOption { type = types.str; default = "5g"; description = "Memory limit for the agents container."; };
    };

    hydramesh = {
      enable = mkOption {
        type = types.bool;
        default = true;
        description = ''
          Run HydraMesh's mesh_mcp (matrix-bridge, streamable HTTP) as a third
          container next to the studio, off the same studio image. The agents'
          CrewAI flow attaches to it as an MCP server over the compose network
          (KILN_MESH_URL) for agent <-> studio messaging. Compose-network only:
          no published port, no egress, no project checkout.
        '';
      };
      port = mkOption {
        type = types.port;
        default = 8765;
        description = "Port mesh_mcp listens on inside the compose network.";
      };
      environment = mkOption {
        type = types.attrsOf types.str;
        default = { };
        example = { DCF_AGENT_NODE_ID = "0x00A6"; DCF_CHANNEL = "studio"; };
        description = ''
          Extra environment for the mesh container — DCF_* settings
          (node id, channel, peers) passed straight through to dcf_node.
        '';
      };
    };

    secrets = {
      anthropicKeyFile = mkOption {
        # A STRING and not types.path: a path-typed option would copy the
        # file into the store at eval time, which is precisely what a secret
        # must never do. sops-nix's config.sops.secrets.<n>.path is a string.
        type = types.nullOr types.str;
        default = null;
        example = "config.sops.secrets.kiln-anthropic.path";
        description = ''
          Runtime file holding the Anthropic API key (a sops-nix path),
          mounted read-only into the agents container. Never an image value,
          never in the store.
        '';
      };
      ollamaKeyFile = mkOption {
        type = types.nullOr types.str;
        default = null;
        example = "config.sops.secrets.kiln-ollama.path";
        description = "File holding the Ollama cloud key, mounted as above.";
      };
    };

    build.maxConcurrentJobs = mkOption {
      type = types.ints.positive;
      default = 2;
      description = ''
        Jobs the studio runs at once, for everyone in the session. Builds run in
        the host's nix daemon, so nix.conf's cores and max-jobs bound each one;
        this bounds how many are queued there at a time.
      '';
    };

    network = {
      subnet = mkOption { type = types.str; default = "172.31.84.0/24"; description = "The compose network's subnet."; };
      gateway = mkOption {
        type = types.str;
        default = "172.31.84.1";
        description = "The compose network's gateway — the address `tailscale serve` connects from, as seen inside the container.";
      };
    };

    egressDomains = mkOption {
      type = types.listOf types.str;
      default = [ ];
      description = ''
        Appended to networking.firewall.strictEgress.allow.domains when that
        option exists (Oligarchy). The studio needs none: builds reach caches
        through the host's daemon, under the host's own rules.
      '';
    };
  };

  config = mkIf cfg.enable (mkMerge [
    {
      assertions = [
        {
          assertion = cfg.user != null;
          message = "custom.kilnStudio.user must name the account whose rootless docker runs the studio.";
        }
        {
          assertion = rootless || config.virtualisation.docker.enable;
          message = ''
            custom.kilnStudio needs docker: virtualisation.docker.rootless.enable
            (Oligarchy's only supported daemon) or virtualisation.docker.enable.
          '';
        }
        {
          assertion = lib.hasPrefix "/" cfg.projectDir;
          message = "custom.kilnStudio.projectDir must be an absolute path.";
        }
        {
          assertion = !cfg.tailscaleServe.enable || config.services.tailscale.enable;
          message = "custom.kilnStudio.tailscaleServe needs services.tailscale.enable — the tailnet is the only way in.";
        }
        {
          assertion = !cfg.agents.enable || agentsImage != null;
          message = ''
            custom.kilnStudio.agents.enable needs the Kiln flake's agents-image
            for this system — the same flake that provides studio-image.
          '';
        }
      ];
      warnings =
        optional (cfg.tailscaleServe.enable && cfg.allowedLogins == [ ])
          "custom.kilnStudio: allowedLogins is empty, so nobody can sign in through the tailnet — only with the token in the unit's log."
        ++ optional (cfg.tailscaleServe.enable && cfg.allowedHosts == [ ])
          "custom.kilnStudio: allowedHosts is empty, so requests addressed to this machine's tailnet name are refused."
        ++ optional (cfg.agents.enable && cfg.secrets.anthropicKeyFile == null)
          "custom.kilnStudio: agents is enabled with no anthropicKeyFile — every Claude role and the Claude Code worker will fail at their first call."
        ++ optional (cfg.agents.enable && cfg.secrets.ollamaKeyFile == null)
          "custom.kilnStudio: agents is enabled with no ollamaKeyFile — the content and validator roles fall back to Claude.";

      system.extraDependencies = [ image.runtime ]
        ++ optional (cfg.agents.enable && agentsImage != null) agentsImage.runtime;

      systemd.user.services.kiln-studio = {
        description = "Kiln Studio (rootless docker compose, ${cfg.projectDir})";
        unitConfig.ConditionUser = cfg.user;
        wantedBy = [ "default.target" ];
        wants = [ "docker.service" ];
        after = [ "docker.service" ];
        environment = optionalAttrs rootless { DOCKER_HOST = "unix://%t/docker.sock"; };
        serviceConfig = {
          ExecStart = up;
          ExecStop = down;
          Restart = "on-failure";
          RestartSec = 15;
          TimeoutStartSec = 900;            # the first start loads the image
        };
      };
    }

    (mkIf cfg.tailscaleServe.enable {
      networking.firewall.interfaces.${tsIface}.allowedTCPPorts = [ servePort ];

      systemd.services.kiln-studio-tailscale-serve = {
        description = "tailscale serve ${scheme}:${toString servePort} -> Kiln Studio on 127.0.0.1:${toString cfg.port}";
        wants = [ "tailscaled.service" "network-online.target" ];
        after = [ "tailscaled.service" "network-online.target" ];
        wantedBy = [ "multi-user.target" ];
        serviceConfig = {
          Type = "oneshot";
          RemainAfterExit = true;
          ExecStart = "${config.services.tailscale.package}/bin/tailscale serve --bg --${scheme}=${toString servePort} http://127.0.0.1:${toString cfg.port}";
          ExecStop = "${config.services.tailscale.package}/bin/tailscale serve --${scheme}=${toString servePort} off";
          # Logged out, or tailscaled not up yet: try again rather than give up.
          Restart = "on-failure";
          RestartSec = 30;
        };
      };
    })

    (optionalAttrs (options.networking.firewall ? strictEgress) {
      networking.firewall.strictEgress.allow.domains =
        cfg.egressDomains ++ lib.optionals cfg.agents.enable agentEgress;
    })
  ]);
}
