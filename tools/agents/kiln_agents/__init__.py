# SPDX-License-Identifier: MIT
"""kiln_agents — the CrewAI runtime for the Kiln studio.

One import side-effect, deliberately at the package root so NOTHING in this
package can be reached without it: CrewAI and OpenTelemetry telemetry are off.
CrewAI's defaults phone home (OTel to telemetry.crewai.com:4319, tracing to
app.crewai.com, a first-run 20 s "view traces?" prompt that hangs a service).
The Oligarchy egress firewall also refuses those domains, so a missed default
would fail loudly — but a tool that waits for a firewall to be correct is a
tool that is wrong. nix/checks/agent-flow.nix asserts these are set.
"""

import os

os.environ.setdefault("CREWAI_DISABLE_TELEMETRY", "true")
os.environ.setdefault("OTEL_SDK_DISABLED", "true")
os.environ.setdefault("CREWAI_TRACING_ENABLED", "false")
# There is deliberately no OPENAI_API_KEY anywhere in the agents container;
# CrewAI's memory/knowledge/planning defaults would reach for OpenAI without
# an explicit alternative, and absence is the guard (see models.py).

__all__ = ["flow", "models", "tools"]
