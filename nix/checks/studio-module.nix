# SPDX-License-Identifier: MIT
#
# nix/checks/studio-module.nix — nixosModules.kiln-studio, booted on nixos-25.11.
#
# The module's claims are about a running machine, so they are checked on one:
#
#   * it evaluates on nixos-25.11 (Oligarchy's release) with rootless docker and
#     a stand-in for Oligarchy's strict-egress option, which it appends to;
#   * the user unit loads the image from the store and brings the studio up
#     under the user's rootless daemon, answering on 127.0.0.1 and published
#     there only;
#   * the container has the limits, read-only root and dropped capabilities
#     the options say;
#   * Tailscale identity is believed from the proxy (a loopback client, which is
#     what `tailscale serve` is) for an allowed login only, and a foreign Host
#     is refused;
#   * the serve port is open on tailscale0 and nowhere else, and the serve unit
#     proxies to the right place;
#   * the agents container (CrewAI runtime + Claude Code worker) runs next to
#     the studio with its own limits, its model keys mounted read-only as
#     files, no published port, and its health endpoint reachable from the
#     studio container over the compose network; the module asked strict-egress
#     for the model providers and nothing else;
#   * the HydraMesh container (mesh_mcp, streamable HTTP :8765) runs off the
#     SAME studio image (`command: ["mesh"]`), locked down and unpublished,
#     with its MCP mount answering over the compose network;
#   * a build started inside the container runs in the host's daemon and its
#     output is visible on both sides;
#   * stopping the unit removes the containers.
{ pkgs2511, kilnModule }:

pkgs2511.testers.runNixOSTest {
  name = "kiln-studio-module";

  nodes.machine = { config, pkgs, lib, ... }: {
    imports = [
      kilnModule
      # Oligarchy's strict-egress, reduced to the one option the module touches.
      ({ lib, ... }: {
        options.networking.firewall.strictEgress.allow.domains = lib.mkOption {
          type = lib.types.listOf lib.types.str;
          default = [ ];
        };
      })
    ];

    virtualisation = {
      memorySize = 4096;
      cores = 4;
      diskSize = 8192;
      # As Oligarchy configures it: 25.11's default docker_28 is marked
      # insecure and refuses to evaluate, so the maintained release is pinned.
      docker.rootless = { enable = true; setSocketVariable = true; package = pkgs.docker_29; };
    };
    users.users.alice = { isNormalUser = true; linger = true; };
    services.tailscale.enable = true;
    systemd.tmpfiles.rules = [ "d /home/alice/project 0755 alice users -" ];
    environment.systemPackages = [ pkgs.busybox pkgs.curl ];
    environment.etc."kiln-test/egress.json".text = builtins.toJSON config.networking.firewall.strictEgress.allow.domains;
    # Stand-ins for the sops-nix paths the module mounts into the agents
    # container: real files, so the bind mounts are real too.
    environment.etc."kiln-test/anthropic-key".text = "test-anthropic-key";
    environment.etc."kiln-test/ollama-key".text = "test-ollama-key";

    custom.kilnStudio = {
      enable = true;
      user = "alice";
      projectDir = "/home/alice/project";
      allowedLogins = [ "alice@example.com" ];
      allowedHosts = [ "machine.example.ts.net" ];
      studio = { cpus = "2"; memory = "1g"; };
      egressDomains = [ "studio.example.org" ];
      agents = { enable = true; cpus = "1"; memory = "768m"; };
      hydramesh.enable = true;   # also the default; named so the mesh subtests have a visible knob
      secrets = {
        anthropicKeyFile = "/etc/kiln-test/anthropic-key";
        ollamaKeyFile = "/etc/kiln-test/ollama-key";
      };
    };
  };

  testScript = ''
    import json

    def as_alice(cmd):
        return machine.succeed(
            "su - alice -c 'DOCKER_HOST=unix:///run/user/$(id -u)/docker.sock " + cmd + "'")

    machine.wait_for_unit("multi-user.target")
    machine.wait_for_unit("docker.service", "alice", timeout=600)
    machine.wait_for_unit("kiln-studio.service", "alice", timeout=900)

    with subtest("the studio answers on loopback"):
        machine.wait_until_succeeds("curl -sf http://127.0.0.1:8420/ | grep -q 'Kiln Studio'", timeout=900)

    with subtest("the container is limited and locked down as configured"):
        c = json.loads(as_alice("docker inspect kiln-studio"))[0]
        hc = c["HostConfig"]
        assert hc["NanoCpus"] == 2 * 10**9, hc["NanoCpus"]
        assert hc["Memory"] == 1 << 30, hc["Memory"]
        assert hc["ReadonlyRootfs"] is True
        assert "ALL" in (hc["CapDrop"] or []), hc["CapDrop"]
        binds = hc["PortBindings"]["8420/tcp"]
        assert [b["HostIp"] for b in binds] == ["127.0.0.1"], binds

    with subtest("tailscale identity only for an allowed login, only for an allowed host"):
        who = machine.succeed("curl -sf -H 'Host: machine.example.ts.net' "
                              "-H 'Tailscale-User-Login: alice@example.com' http://127.0.0.1:8420/api/whoami")
        assert json.loads(who) == {"user": "alice@example.com", "via": "tailscale"}, who
        machine.fail("curl -sf -H 'Host: machine.example.ts.net' "
                     "-H 'Tailscale-User-Login: mallory@example.com' http://127.0.0.1:8420/api/whoami")
        machine.fail("curl -sf -H 'Host: attacker.example' "
                     "-H 'Tailscale-User-Login: alice@example.com' http://127.0.0.1:8420/api/whoami")

    with subtest("the serve port is open on tailscale0 only, and serve proxies to the studio"):
        rules = machine.succeed("iptables-save")
        opened = [l for l in rules.splitlines() if "--dport 80 " in l and "-j nixos-fw-accept" in l]
        assert opened and all("-i tailscale0" in l for l in opened), opened
        unit = machine.succeed("systemctl cat kiln-studio-tailscale-serve.service")
        assert "serve --bg --http=80 http://127.0.0.1:8420" in unit, unit

    with subtest("the module appended to strict-egress"):
        egress = json.loads(machine.succeed("cat /etc/kiln-test/egress.json"))
        assert "studio.example.org" in egress
        for d in ("api.anthropic.com", "ollama.com"):
            assert d in egress, (d, egress)

    with subtest("the agents container runs alongside the studio, limited and locked down"):
        # NB: no /api/whoami liveness probe here — whoami needs an identity
        # (a tailscale header or the token) and would 401 forever. The studio
        # being up is already established by the first subtest; the agents
        # container existing is the only new thing this subtest waits on.
        machine.wait_until_succeeds(
            "su - alice -c 'DOCKER_HOST=unix:///run/user/$(id -u)/docker.sock docker inspect kiln-agents' >/dev/null",
            timeout=900)
        a = json.loads(as_alice("docker inspect kiln-agents"))[0]
        ahc = a["HostConfig"]
        assert ahc["NanoCpus"] == 1 * 10**9, ahc["NanoCpus"]
        assert ahc["Memory"] == 768 * 1024 * 1024, ahc["Memory"]
        assert ahc["ReadonlyRootfs"] is True
        assert "ALL" in (ahc["CapDrop"] or []), ahc["CapDrop"]
        assert a["Config"]["ExposedPorts"] == {"8600/tcp": {}}, a["Config"]["ExposedPorts"]
        binds = ahc["Binds"]
        assert any(b.startswith("/etc/kiln-test/anthropic-key:") and b.endswith(":ro") for b in binds), binds
        assert any(b.startswith("/etc/kiln-test/ollama-key:") and b.endswith(":ro") for b in binds), binds
        env = {e.split("=", 1)[0]: e.split("=", 1)[1] for e in a["Config"]["Env"]}
        assert env["ANTHROPIC_API_KEY_FILE"] == "/run/secrets/anthropic-key", env
        assert "ANTHROPIC_API_KEY" not in env, env
        assert "test-anthropic-key" not in json.dumps(a)
        assert "test-ollama-key" not in json.dumps(a)
        sc = json.loads(as_alice("docker inspect kiln-studio"))[0]
        assert "ANTHROPIC_API_KEY" not in {e.split("=", 1)[0]: e.split("=", 1)[1] for e in sc["Config"]["Env"]}
        assert "test-anthropic-key" not in json.dumps(sc)
        assert env["KILN_STUDIO_URL"] == "http://kiln-studio:8420", env
        assert env["KILN_MESH_URL"] == "http://kiln-mesh:8765/mcp", env
        # No published port: the agents service is reachable on the compose
        # network only.
        assert not ahc["PortBindings"], ahc["PortBindings"]

    with subtest("the agents service answers over the compose network, from the studio container"):
        # Retried: serve.py imports CrewAI before it binds 8600, which takes a
        # while under a 768 MB limit — one shot would race it. The .decode()
        # matters too: read() returns bytes and print() would show Python's
        # b'...' repr, which json.loads refuses.
        health = machine.wait_until_succeeds(
            "su - alice -c \"DOCKER_HOST=unix:///run/user/\\$(id -u)/docker.sock docker exec kiln-studio python3 -c 'import urllib.request; print(urllib.request.urlopen(\\\"http://kiln-agents:8600/healthz\\\").read().decode())'\"",
            timeout=600)
        assert json.loads(health) == {"ok": True}, health

    with subtest("the mesh container runs off the studio image, locked down, unpublished"):
        machine.wait_until_succeeds(
            "su - alice -c 'DOCKER_HOST=unix:///run/user/$(id -u)/docker.sock docker inspect kiln-mesh' >/dev/null",
            timeout=900)
        m = json.loads(as_alice("docker inspect kiln-mesh"))[0]
        mhc = m["HostConfig"]
        assert mhc["ReadonlyRootfs"] is True
        assert "ALL" in (mhc["CapDrop"] or []), mhc["CapDrop"]
        assert not mhc["PortBindings"], mhc["PortBindings"]
        assert m["Config"]["Image"] == json.loads(as_alice("docker inspect kiln-studio"))[0]["Config"]["Image"], \
            "the mesh service must run the SAME image as the studio"
        assert m["Config"]["Cmd"] == ["mesh"], m["Config"]["Cmd"]
        menv = {e.split("=", 1)[0]: e.split("=", 1)[1] for e in m["Config"]["Env"]}
        assert menv["DCF_MCP_HTTP_HOST"] == "0.0.0.0", menv
        assert menv["DCF_MCP_HTTP_PORT"] == "8765", menv

    with subtest("the mesh answers MCP over the compose network, from the studio container"):
        # A bare GET on the streamable-HTTP mount is refused with a 4xx — that
        # refusal is the assertion: an answering MCP transport says "missing
        # session / bad request", and a wrong mount would say 404, so both
        # liveness and the /mcp path are proven by one probe. http.client
        # rather than urllib because it does not RAISE on 4xx, which keeps
        # the probe a single shell-safe line through the su/docker quoting.
        code = machine.wait_until_succeeds(
            "su - alice -c \"DOCKER_HOST=unix:///run/user/\\$(id -u)/docker.sock docker exec kiln-studio python3 -c 'import http.client; c=http.client.HTTPConnection(\\\"kiln-mesh\\\", 8765); c.request(\\\"GET\\\", \\\"/mcp\\\"); print(c.getresponse().status)'\"",
            timeout=600)
        code = code.strip()
        assert code.isdigit() and 400 <= int(code) < 500 and int(code) != 404, code

    with subtest("a build inside the container runs in the host's daemon"):
        machine.succeed("""cat > /home/alice/project/probe.nix <<'EOF'
    derivation {
      name = "kiln-studio-probe";
      system = builtins.currentSystem;
      builder = "''${builtins.storePath "${pkgs2511.busybox}"}/bin/sh";
      args = [ "-c" "echo built-by-the-host > $out" ];
    }
    EOF""")
        out = as_alice("docker exec kiln-studio nix build --no-link --print-out-paths -f /home/alice/project/probe.nix").strip()
        assert out.startswith("/nix/store/"), out
        assert as_alice(f"docker exec kiln-studio cat {out}").strip() == "built-by-the-host"
        assert machine.succeed(f"cat {out}").strip() == "built-by-the-host"

    with subtest("stopping the unit removes all three containers"):
        machine.succeed("systemctl --machine=alice@ --user stop kiln-studio.service")
        # name=kiln- matches kiln-studio, kiln-agents and kiln-mesh alike; the
        # compose `down` removes what the config declares, --remove-orphans
        # catches anything a later config change stopped declaring.
        machine.wait_until_succeeds("test -z \"$(su - alice -c 'DOCKER_HOST=unix:///run/user/$(id -u)/docker.sock docker ps -aq --filter name=kiln-')\"", timeout=120)
  '';
}
