<!--
SPDX-FileCopyrightText: 2026 ypb6010-ux
SPDX-License-Identifier: MPL-2.0
-->

# Security Policy

FieldRuntime is an **operational-technology (OT) runtime**: deployed, it
can read from and **write to** PLCs and industrial actuators. A
vulnerability here can have physical consequences. Please treat security
reports accordingly.

## Reporting a vulnerability

**Do not open a public issue for security vulnerabilities.**

Report privately via **GitHub private vulnerability reporting**:
on the repository, go to **Security → "Report a vulnerability"**. This
opens a private channel visible only to the maintainers.

Please include: affected version/commit, build configuration, a
description of the issue, reproduction steps or a proof of concept, and the
potential impact (especially any path that could cause an unintended write
to a field device).

We aim to acknowledge a report within a few business days and to agree on
a disclosure timeline with the reporter. Please allow a reasonable
remediation window before public disclosure.

## Scope notes for integrators

FieldRuntime is a component, not a finished secured product. When you ship
it in a gateway, you are responsible for the deployment security boundary,
including at least:

- Do not expose configuration/control interfaces to untrusted networks;
  bind management to a trusted LAN/management port.
- Require authentication, and prefer TLS, for any web/IPC control plane.
- Separate the configuration/management network from the control network.
- Audit every write-to-field operation; gate high-risk writes behind
  confirmation or role-based access.
- Sandbox and resource-limit any scripting (e.g. the Lua codec) and only
  accept scripts from trusted operators.

These are integrator responsibilities and are not guaranteed by the
library itself.
